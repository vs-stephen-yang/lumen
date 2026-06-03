// WebTransportServer adapter loopback test.
//
// Drives a real quiche WebTransport client that establishes a session against
// WebTransportServer (the abstract TransportServer backed by quiche) and sends
// a two-fragment video frame as WT datagrams:
//   [session_id varint][channel_id:u8][MediaPacketHeader:28][fragment]
//
// Asserts that the adapter routes by channel_id, reassembles the fragments,
// and delivers [MediaPacketHeader:28][frame] through the video channel's
// DataReceivedCallback with the header and payload intact.

#include "transport/web_transport_server.h"

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"
#include "transport/wt_varint.h"

#include <quiche.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

using namespace lumen;
using namespace std::chrono_literals;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg);     \
                   ++failed; return; }                                         \
} while (0)

constexpr size_t kMaxDgram = 1350;
constexpr size_t kCidLen = 16;
constexpr uint64_t kSsrc = 0xCAFEBEEF;
constexpr uint64_t kFrameIndex = 42;

bool RandBytes(uint8_t* p, size_t n) {
    return BCryptGenRandom(nullptr, p, static_cast<ULONG>(n),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string RepoRoot() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof(path));
    std::string s = path;
    for (auto& c : s) if (c == '\\') c = '/';
    for (int i = 0; i < 4; ++i) s = s.substr(0, s.find_last_of('/'));
    return s;
}

// Build a WT datagram for one fragment of the video channel (channel_id 0):
//   [session_id varint][channel_id:u8][MediaPacketHeader:28][fragment]
std::vector<uint8_t> BuildVideoDatagram(uint64_t session_id,
                                         const MediaPacketHeader& h,
                                         const uint8_t* payload,
                                         size_t payload_len) {
    uint8_t vbuf[8];
    const size_t vlen = VarintEncode(session_id, vbuf, sizeof(vbuf));
    std::vector<uint8_t> out;
    out.insert(out.end(), vbuf, vbuf + vlen);
    out.push_back(static_cast<uint8_t>(ChannelType::kVideo));
    const size_t hoff = out.size();
    out.resize(out.size() + MediaPacketHeader::kSerializedSize);
    h.Serialize(out.data() + hoff);
    out.insert(out.end(), payload, payload + payload_len);
    return out;
}

void test_adapter_datagram_reassembly() {
    const std::string root = RepoRoot();

    // ── Server: WebTransportServer behind the abstract interface ──────
    WebTransportServer server;

    std::atomic<bool> got_frame{false};
    std::vector<uint8_t> recv_buf;  // delivered [header][frame]
    std::mutex rmu;
    std::shared_ptr<TransportConnection> keep;

    auto on_incoming = [&](std::shared_ptr<TransportConnection> conn) {
        if (auto* ch = conn->GetChannel(ChannelType::kVideo)) {
            ch->SetReceiveCallback([&](const uint8_t* d, size_t n) {
                std::lock_guard<std::mutex> lock(rmu);
                recv_buf.assign(d, d + n);
                got_frame.store(true);
            });
        }
        keep = std::move(conn);
    };

    TransportConfig tcfg;
    tcfg.address       = "127.0.0.1";
    tcfg.port          = 0;
    tcfg.tls.cert_path = root + "/third_party/quiche/quiche/examples/cert.crt";
    tcfg.tls.key_path  = root + "/third_party/quiche/quiche/examples/cert.key";

    auto init = server.Initialize(tcfg);
    CHECK(init.ok(), init.error().message.c_str());
    CHECK(server.Start(on_incoming).ok(), "server.Start");
    const uint16_t port = server.GetListenPort();
    CHECK(port != 0, "ephemeral port");

    // The original frame, split into two fragments.
    std::vector<uint8_t> frame(1500);
    for (size_t i = 0; i < frame.size(); ++i)
        frame[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
    const size_t split = 800;

    MediaPacketHeader h0;
    h0.ssrc = static_cast<uint32_t>(kSsrc);
    h0.frame_index = kFrameIndex;
    h0.timestamp_us = 123456;
    h0.fragment_index = 0;
    h0.fragment_count = 2;
    h0.SetKeyframe(true);
    h0.SetLastFragment(false);

    MediaPacketHeader h1 = h0;
    h1.fragment_index = 1;
    h1.SetLastFragment(true);

    // ── Client setup (quiche WebTransport) ────────────────────────────
    quiche_config* ccfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    static const uint8_t kAlpnH3[] = {0x02, 'h', '3'};
    quiche_config_set_application_protos(ccfg, kAlpnH3, sizeof(kAlpnH3));
    quiche_config_set_max_idle_timeout(ccfg, 5000);
    quiche_config_set_max_recv_udp_payload_size(ccfg, kMaxDgram);
    quiche_config_set_max_send_udp_payload_size(ccfg, kMaxDgram);
    quiche_config_set_initial_max_data(ccfg, 10'000'000);
    quiche_config_set_initial_max_stream_data_bidi_local(ccfg, 1'000'000);
    quiche_config_set_initial_max_stream_data_bidi_remote(ccfg, 1'000'000);
    quiche_config_set_initial_max_stream_data_uni(ccfg, 1'000'000);
    quiche_config_set_initial_max_streams_bidi(ccfg, 100);
    quiche_config_set_initial_max_streams_uni(ccfg, 100);
    quiche_config_enable_dgram(ccfg, true, 1024, 1024);
    quiche_config_verify_peer(ccfg, false);

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in cli_local = {AF_INET, 0, {}};
    cli_local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&cli_local), sizeof(cli_local));
    int cli_local_len = sizeof(cli_local);
    getsockname(s, reinterpret_cast<sockaddr*>(&cli_local), &cli_local_len);
    sockaddr_in srv = {AF_INET, htons(port), {}};
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    uint8_t scid[kCidLen]; RandBytes(scid, kCidLen);
    quiche_conn* qc = quiche_connect(
        "localhost", scid, kCidLen,
        reinterpret_cast<const sockaddr*>(&cli_local), sizeof(cli_local),
        reinterpret_cast<const sockaddr*>(&srv), sizeof(srv), ccfg);
    CHECK(qc != nullptr, "quiche_connect");

    quiche_h3_config* h3cfg = quiche_h3_config_new();
    quiche_h3_config_enable_extended_connect(h3cfg, true);

    quiche_h3_conn* h3 = nullptr;
    bool sent_connect = false;
    int64_t connect_stream = -1;

    auto pump_send = [&]() {
        uint8_t out[kMaxDgram];
        while (true) {
            quiche_send_info si = {};
            ssize_t w = quiche_conn_send(qc, out, sizeof(out), &si);
            if (w == QUICHE_ERR_DONE) break;
            if (w < 0) return false;
            sendto(s, reinterpret_cast<const char*>(out),
                   static_cast<int>(w), 0,
                   reinterpret_cast<const sockaddr*>(&si.to),
                   static_cast<int>(si.to_len));
        }
        return true;
    };

    auto deadline = std::chrono::steady_clock::now() + 5s;
    uint8_t in_buf[65535];
    while (std::chrono::steady_clock::now() < deadline && !got_frame.load()) {
        CHECK(pump_send(), "client send pump");

        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        timeval tv = {0, 100 * 1000};
        if (::select(0, &fds, nullptr, nullptr, &tv) > 0) {
            sockaddr_in from = {};
            int from_len = sizeof(from);
            int n = ::recvfrom(s, reinterpret_cast<char*>(in_buf),
                                sizeof(in_buf), 0,
                                reinterpret_cast<sockaddr*>(&from),
                                &from_len);
            if (n > 0) {
                quiche_recv_info ri = {
                    reinterpret_cast<sockaddr*>(&from), from_len,
                    reinterpret_cast<sockaddr*>(&cli_local),
                    static_cast<socklen_t>(cli_local_len),
                };
                quiche_conn_recv(qc, in_buf, n, &ri);
            }
        } else {
            quiche_conn_on_timeout(qc);
        }

        if (quiche_conn_is_established(qc) && !h3) {
            h3 = quiche_h3_conn_new_with_transport(qc, h3cfg);
        }

        if (h3 && !sent_connect &&
            quiche_h3_extended_connect_enabled_by_peer(h3)) {
            quiche_h3_header req[] = {
                {(const uint8_t*)":method",    7, (const uint8_t*)"CONNECT", 7},
                {(const uint8_t*)":protocol",  9, (const uint8_t*)"webtransport", 12},
                {(const uint8_t*)":scheme",    7, (const uint8_t*)"https", 5},
                {(const uint8_t*)":authority", 10, (const uint8_t*)"localhost", 9},
                {(const uint8_t*)":path",      5, (const uint8_t*)"/lumen", 6},
            };
            connect_stream = quiche_h3_send_request(h3, qc, req, 5,
                                                     /*fin=*/false);
            CHECK(connect_stream >= 0, "send_request CONNECT");
            sent_connect = true;
        }

        if (h3) {
            quiche_h3_event* ev = nullptr;
            while (true) {
                int64_t sid = quiche_h3_conn_poll(h3, qc, &ev);
                if (sid < 0) break;
                quiche_h3_event_free(ev);
            }
        }

        // Once the session stream is known, push the two video fragments.
        // The session_id is the CONNECT stream id. Re-send every iteration:
        // datagrams sent before the server records the session are dropped,
        // and the reassembler deduplicates the redundant fragments.
        if (sent_connect && connect_stream >= 0) {
            auto sid = static_cast<uint64_t>(connect_stream);
            auto d0 = BuildVideoDatagram(sid, h0, frame.data(), split);
            auto d1 = BuildVideoDatagram(sid, h1, frame.data() + split,
                                         frame.size() - split);
            quiche_conn_dgram_send(qc, d0.data(), d0.size());
            quiche_conn_dgram_send(qc, d1.data(), d1.size());
            pump_send();
        }
    }

    CHECK(got_frame.load(), "adapter never delivered the reassembled frame");

    std::vector<uint8_t> delivered;
    {
        std::lock_guard<std::mutex> lock(rmu);
        delivered = recv_buf;
    }
    CHECK(delivered.size() == MediaPacketHeader::kSerializedSize + frame.size(),
          "delivered size mismatch");

    MediaPacketHeader got;
    CHECK(MediaPacketHeader::Deserialize(delivered.data(), delivered.size(),
                                          got),
          "header deserialize");
    CHECK(got.frame_index == kFrameIndex, "frame_index mismatch");
    CHECK(got.IsKeyframe(), "keyframe flag lost");
    CHECK(std::memcmp(delivered.data() + MediaPacketHeader::kSerializedSize,
                       frame.data(), frame.size()) == 0,
          "frame payload mismatch");

    std::printf("[adapter] delivered reassembled frame: %zu bytes, "
                "frame_index=%llu keyframe=%d\n",
                frame.size(), (unsigned long long)got.frame_index,
                got.IsKeyframe());

    quiche_h3_conn_free(h3);
    quiche_conn_free(qc);
    quiche_h3_config_free(h3cfg);
    quiche_config_free(ccfg);
    closesocket(s);
    server.Shutdown();
    ++passed;
}

}  // namespace

int main() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    test_adapter_datagram_reassembly();
    WSACleanup();
    std::printf("web_transport_adapter_test: %d passed, %d failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
