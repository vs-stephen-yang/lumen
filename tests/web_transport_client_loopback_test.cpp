// Native WebTransportClient -> WebTransportServer loopback over real QUIC.
//
// Restores native<->native coverage (previously transport_quic_test on msquic)
// against the quiche stack, exercised entirely through the abstract
// TransportClient / TransportServer / TransportConnection / TransportChannel
// interfaces. The client sends a two-fragment video frame on its video channel;
// the server's connection must reassemble and deliver [header][frame].

#include "transport/web_transport_server.h"
#include "transport/web_transport_client.h"

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace lumen;
using namespace std::chrono_literals;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg);     \
                   ++failed; return; }                                         \
} while (0)

constexpr uint64_t kFrameIndex = 7;

std::string RepoRoot() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof(path));
    std::string s = path;
    for (auto& c : s) if (c == '\\') c = '/';
    for (int i = 0; i < 4; ++i) s = s.substr(0, s.find_last_of('/'));
    return s;
}

void test_client_to_server_frame() {
    const std::string root = RepoRoot();

    // ── Server ──────────────────────────────────────────────────────
    WebTransportServer server;

    std::atomic<bool> got_frame{false};
    std::vector<uint8_t> recv_buf;
    std::shared_ptr<TransportConnection> server_conn;
    std::mutex smu;

    auto on_incoming = [&](std::shared_ptr<TransportConnection> conn) {
        if (auto* ch = conn->GetChannel(ChannelType::kVideo)) {
            ch->SetReceiveCallback([&](const uint8_t* d, size_t n) {
                std::lock_guard<std::mutex> lock(smu);
                recv_buf.assign(d, d + n);
                got_frame.store(true);
            });
        }
        std::lock_guard<std::mutex> lock(smu);
        server_conn = std::move(conn);
    };

    TransportConfig scfg;
    scfg.address       = "127.0.0.1";
    scfg.port          = 0;
    scfg.tls.cert_path = root + "/third_party/quiche/quiche/examples/cert.crt";
    scfg.tls.key_path  = root + "/third_party/quiche/quiche/examples/cert.key";

    CHECK(server.Initialize(scfg).ok(), "server.Initialize");
    CHECK(server.Start(on_incoming).ok(), "server.Start");
    const uint16_t port = server.GetListenPort();
    CHECK(port != 0, "ephemeral port");

    // ── Client ──────────────────────────────────────────────────────
    WebTransportClient client;
    std::atomic<bool> client_ready{false};
    std::shared_ptr<TransportConnection> client_conn;
    std::mutex cmu;

    TransportConfig ccfg;
    ccfg.address = "127.0.0.1";
    ccfg.port    = port;
    ccfg.tls.verify_peer = false;  // self-signed example cert

    CHECK(client.Initialize(ccfg).ok(), "client.Initialize");
    CHECK(client.Connect([&](std::shared_ptr<TransportConnection> conn) {
        std::lock_guard<std::mutex> lock(cmu);
        client_conn = std::move(conn);
        client_ready.store(true);
    }).ok(), "client.Connect");

    // Wait for the session to come up on the client side.
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!client_ready.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }
    CHECK(client_ready.load(), "client session never established");

    std::shared_ptr<TransportConnection> cc;
    {
        std::lock_guard<std::mutex> lock(cmu);
        cc = client_conn;
    }
    CHECK(cc != nullptr, "no client connection");

    // The frame to ship (channel fragments it into two datagrams).
    std::vector<uint8_t> frame(1500);
    for (size_t i = 0; i < frame.size(); ++i)
        frame[i] = static_cast<uint8_t>((i * 11 + 5) & 0xFF);

    SendOptions opts;
    opts.frame_index = kFrameIndex;
    opts.timestamp_us = 123456;
    opts.is_keyframe = true;

    // Datagrams are unreliable; resend until the server reports the frame
    // (loopback loss is ~nil, but this keeps the test robust).
    auto* vch = cc->GetChannel(ChannelType::kVideo);
    CHECK(vch != nullptr, "no client video channel");
    while (!got_frame.load() && std::chrono::steady_clock::now() < deadline) {
        vch->Send(frame.data(), frame.size(), opts);
        std::this_thread::sleep_for(50ms);
    }

    CHECK(got_frame.load(), "server never received the frame");

    std::vector<uint8_t> delivered;
    {
        std::lock_guard<std::mutex> lock(smu);
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

    // Stats should be wired on both ends.
    TransportStats cs = cc->GetStats();
    CHECK(cs.bytes_sent > 0, "client GetStats reported zero bytes_sent");

    std::printf("[client->server] frame delivered: %zu bytes, frame_index=%llu "
                "keyframe=%d; client rtt_us=%llu bytes_sent=%llu\n",
                frame.size(), (unsigned long long)got.frame_index,
                got.IsKeyframe(), (unsigned long long)cs.rtt_us,
                (unsigned long long)cs.bytes_sent);

    client.Shutdown();
    server.Shutdown();
    ++passed;
}

}  // namespace

int main() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    test_client_to_server_frame();
    WSACleanup();
    std::printf("web_transport_client_loopback_test: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
