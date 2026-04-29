// Phase 2.1 smoke test: QuicheServer can Initialize, Start, accept a
// QUIC handshake, and Stop cleanly.
//
// We use quiche's own client side (linked from the same lib) to drive
// the handshake, so this is a real end-to-end QUIC TLS exchange — not
// just lifecycle. The bundled examples/cert.{crt,key} pair acts as the
// dev cert; the client disables peer cert verification.

#include "transport/quiche_server.h"

#include <quiche.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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

// Locate the repo root by walking up from the binary.
std::string RepoRoot() {
    // The binary is at build/tests/Debug/quiche_server_smoke_test.exe.
    // Walk up three levels.
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof(path));
    std::string s = path;
    for (auto& c : s) if (c == '\\') c = '/';
    auto last = s.find_last_of('/');
    s = s.substr(0, last);                        // .../Debug
    last = s.find_last_of('/'); s = s.substr(0, last);  // .../tests
    last = s.find_last_of('/'); s = s.substr(0, last);  // .../build
    last = s.find_last_of('/'); s = s.substr(0, last);  // .../lumen
    return s;
}

void test_lifecycle_and_handshake() {
    const std::string root = RepoRoot();
    const std::string cert = root + "/third_party/quiche/quiche/examples/cert.crt";
    const std::string key  = root + "/third_party/quiche/quiche/examples/cert.key";

    QuicheServer server;
    QuicheServerConfig cfg;
    cfg.address = "127.0.0.1";
    cfg.port    = 0;
    cfg.cert_pem_path = cert;
    cfg.key_pem_path  = key;
    // The bundled cert.crt was issued for HTTP/0.9 demos; align ALPN
    // to a value the example certificate accepts. We're only proving
    // the QUIC handshake here, not negotiating HTTP/3.
    cfg.alpn = {0x05, 'h', 'q', '-', '2', '9'};

    auto init = server.Initialize(cfg);
    CHECK(init.ok(), init.error().message.c_str());

    auto start = server.Start();
    CHECK(start.ok(), start.error().message.c_str());

    const uint16_t port = server.GetListenPort();
    CHECK(port != 0, "ephemeral port not assigned");
    std::printf("[server] udp/127.0.0.1:%u\n", port);

    // ── Build a quiche client and drive a handshake ─────────────────
    quiche_config* ccfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    CHECK(ccfg != nullptr, "client quiche_config_new");
    quiche_config_set_application_protos(ccfg, cfg.alpn.data(),
                                          cfg.alpn.size());
    quiche_config_set_max_idle_timeout(ccfg, 5000);
    quiche_config_set_max_recv_udp_payload_size(ccfg, kMaxDgram);
    quiche_config_set_max_send_udp_payload_size(ccfg, kMaxDgram);
    quiche_config_set_initial_max_data(ccfg, 10'000'000);
    quiche_config_set_initial_max_stream_data_bidi_local(ccfg, 1'000'000);
    quiche_config_set_initial_max_streams_bidi(ccfg, 100);
    quiche_config_verify_peer(ccfg, false);  // self-signed cert

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(s != INVALID_SOCKET, "client socket");

    sockaddr_in cli_local = {AF_INET, 0, {}};
    cli_local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(s, reinterpret_cast<sockaddr*>(&cli_local),
                 sizeof(cli_local)) == 0, "client bind");
    int cli_local_len = sizeof(cli_local);
    getsockname(s, reinterpret_cast<sockaddr*>(&cli_local), &cli_local_len);

    sockaddr_in srv_addr = {AF_INET, htons(port), {}};
    srv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    uint8_t scid[kCidLen];
    CHECK(RandBytes(scid, kCidLen), "rng");

    quiche_conn* qc = quiche_connect(
        "localhost", scid, kCidLen,
        reinterpret_cast<const sockaddr*>(&cli_local), sizeof(cli_local),
        reinterpret_cast<const sockaddr*>(&srv_addr),  sizeof(srv_addr),
        ccfg);
    CHECK(qc != nullptr, "quiche_connect");

    // Pump until established or timeout.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    bool established = false;

    uint8_t out[kMaxDgram];
    uint8_t in_buf[65535];

    while (std::chrono::steady_clock::now() < deadline && !established) {
        // Send everything quiche has queued.
        while (true) {
            quiche_send_info si = {};
            ssize_t w = quiche_conn_send(qc, out, sizeof(out), &si);
            if (w == QUICHE_ERR_DONE) break;
            if (w < 0) { CHECK(false, "client quiche_conn_send error"); }
            sendto(s, reinterpret_cast<const char*>(out), static_cast<int>(w),
                   0, reinterpret_cast<const sockaddr*>(&si.to),
                   static_cast<int>(si.to_len));
        }

        // Wait briefly for inbound packets.
        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        timeval tv = {0, 100 * 1000};
        if (::select(0, &fds, nullptr, nullptr, &tv) > 0) {
            sockaddr_in from = {};
            int from_len = sizeof(from);
            int n = recvfrom(s, reinterpret_cast<char*>(in_buf),
                              sizeof(in_buf), 0,
                              reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0) {
                quiche_recv_info ri = {
                    reinterpret_cast<sockaddr*>(&from), from_len,
                    reinterpret_cast<sockaddr*>(&cli_local),
                    static_cast<socklen_t>(cli_local_len),
                };
                quiche_conn_recv(qc, in_buf, n, &ri);
            }
        } else {
            // Trigger any timers that may have fired.
            quiche_conn_on_timeout(qc);
        }

        if (quiche_conn_is_established(qc)) established = true;
    }

    CHECK(established, "client failed to establish QUIC handshake");
    std::printf("[client] established\n");

    // Verify server saw the connection.
    CHECK(server.GetConnectionCount() >= 1,
          "server should have at least one active connection");

    quiche_conn_free(qc);
    quiche_config_free(ccfg);
    closesocket(s);
    server.Shutdown();
    ++passed;
}

}  // namespace

int main() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    test_lifecycle_and_handshake();

    WSACleanup();
    std::printf("quiche_server_smoke_test: %d passed, %d failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
