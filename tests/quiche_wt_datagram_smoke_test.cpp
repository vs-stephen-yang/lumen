// Phase 2.4 smoke test: WebTransport datagram round-trip on an
// established session. The server pushes a datagram with the session_id
// varint prefix; the client receives it via quiche_conn_dgram_recv,
// strips the prefix, and asserts the payload matches.

#include "transport/quiche_server.h"
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
#include <string>
#include <thread>
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

void test_datagram_round_trip() {
    const std::string root = RepoRoot();
    QuicheServerConfig cfg;
    cfg.address = "127.0.0.1";
    cfg.port    = 0;
    cfg.cert_pem_path = root + "/third_party/quiche/quiche/examples/cert.crt";
    cfg.key_pem_path  = root + "/third_party/quiche/quiche/examples/cert.key";

    QuicheServer server;

    // When a session establishes, push a datagram on it. The body is
    // a known string the client will assert against.
    std::string established_conn_id;
    std::atomic<bool> session_ready{false};
    server.SetOnWebTransportSession(
        [&](const WebTransportSessionInfo& info) {
            established_conn_id = info.conn_id;
            // Send the datagram from a separate thread so we don't block
            // the recv thread that called us.
            std::thread([&server, info]() {
                std::this_thread::sleep_for(20ms);
                static const uint8_t kPayload[] = "hello-from-server";
                auto rc = server.SendWebTransportDatagram(
                    info.conn_id, info.session_id, kPayload,
                    sizeof(kPayload) - 1);
                if (!rc.ok()) {
                    std::fprintf(stderr, "[server] dgram send failed: %s\n",
                                  rc.error().message.c_str());
                }
            }).detach();
            session_ready.store(true);
        });

    auto init = server.Initialize(cfg);
    CHECK(init.ok(), init.error().message.c_str());
    CHECK(server.Start().ok(), "server.Start");
    const uint16_t port = server.GetListenPort();
    CHECK(port != 0, "ephemeral port");

    // ── Client setup ────────────────────────────────────────────────
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
    bool got_dgram = false;
    std::string got_payload;

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
    while (std::chrono::steady_clock::now() < deadline && !got_dgram) {
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

        // Drain client-side datagrams.
        while (true) {
            uint8_t buf[1500];
            ssize_t n = quiche_conn_dgram_recv(qc, buf, sizeof(buf));
            if (n <= 0) break;
            uint64_t sid = 0;
            const size_t consumed = VarintDecode(buf,
                static_cast<size_t>(n), &sid);
            if (consumed == 0) continue;
            got_payload.assign(reinterpret_cast<char*>(buf + consumed),
                                static_cast<size_t>(n) - consumed);
            got_dgram = true;
            break;
        }
    }

    CHECK(session_ready.load(), "server never saw the session");
    CHECK(got_dgram, "client never received a datagram");
    CHECK(got_payload == "hello-from-server",
           ("payload mismatch: \"" + got_payload + "\"").c_str());
    std::printf("[client] received WT datagram: \"%s\"\n", got_payload.c_str());

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
    test_datagram_round_trip();
    WSACleanup();
    std::printf("quiche_wt_datagram_smoke_test: %d passed, %d failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
