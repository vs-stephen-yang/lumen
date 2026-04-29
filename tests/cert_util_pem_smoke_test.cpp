// Phase 2.5 smoke test: cert_util generates an ECDSA P-256 cert,
// exports it as PEM (cert + private key), QuicheServer loads the PEMs,
// and a real QUIC handshake completes. Proves the PEM export pipeline
// actually produces files that BoringSSL/quiche accepts.

#include "common/cert_util.h"
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

int passed = 0, failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) {                                   \
    std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg); ++failed; return; } \
} while (0)

constexpr size_t kMaxDgram = 1350;
constexpr size_t kCidLen = 16;

bool RandBytes(uint8_t* p, size_t n) {
    return BCryptGenRandom(nullptr, p, static_cast<ULONG>(n),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string TempFile(const char* tag) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    char name[MAX_PATH];
    GetTempFileNameA(dir, tag, 0, name);
    return std::string(name);
}

void test_pem_round_trip_with_real_handshake() {
    auto cert = CreateSelfSignedCert(L"LumenPemTestKey",
                                       L"lumen-pem-test", 14);
    CHECK(cert.valid, "cert generation");
    std::printf("[cert] sha256=%s\n", Sha256ToHex(cert.sha256_der).c_str());

    const std::string cert_path = TempFile("lcrt");
    const std::string key_path  = TempFile("lkey");
    CHECK(WriteCertAndKeyPemFiles(cert, cert_path, key_path),
           "WriteCertAndKeyPemFiles");
    std::printf("[cert] wrote %s + %s\n", cert_path.c_str(),
                 key_path.c_str());

    QuicheServer server;
    QuicheServerConfig cfg;
    cfg.address = "127.0.0.1";
    cfg.port    = 0;
    cfg.cert_pem_path = cert_path;
    cfg.key_pem_path  = key_path;
    auto init = server.Initialize(cfg);
    CHECK(init.ok(), init.error().message.c_str());
    CHECK(server.Start().ok(), "server.Start");

    // ── Real client handshake against our self-gen PEM ──────────────
    quiche_config* ccfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    static const uint8_t kAlpnH3[] = {0x02, 'h', '3'};
    quiche_config_set_application_protos(ccfg, kAlpnH3, sizeof(kAlpnH3));
    quiche_config_set_max_idle_timeout(ccfg, 5000);
    quiche_config_set_max_recv_udp_payload_size(ccfg, kMaxDgram);
    quiche_config_set_max_send_udp_payload_size(ccfg, kMaxDgram);
    quiche_config_set_initial_max_data(ccfg, 10'000'000);
    quiche_config_set_initial_max_stream_data_bidi_local(ccfg, 1'000'000);
    quiche_config_set_initial_max_streams_bidi(ccfg, 100);
    quiche_config_set_initial_max_streams_uni(ccfg, 100);
    quiche_config_verify_peer(ccfg, false);

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in cli = {AF_INET, 0, {}};
    cli.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, reinterpret_cast<sockaddr*>(&cli), sizeof(cli));
    int cli_len = sizeof(cli);
    getsockname(s, reinterpret_cast<sockaddr*>(&cli), &cli_len);
    sockaddr_in srv = {AF_INET, htons(server.GetListenPort()), {}};
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    uint8_t scid[kCidLen]; RandBytes(scid, kCidLen);
    quiche_conn* qc = quiche_connect(
        "localhost", scid, kCidLen,
        reinterpret_cast<const sockaddr*>(&cli), sizeof(cli),
        reinterpret_cast<const sockaddr*>(&srv), sizeof(srv), ccfg);
    CHECK(qc != nullptr, "quiche_connect");

    auto deadline = std::chrono::steady_clock::now() + 5s;
    uint8_t in_buf[65535], out[kMaxDgram];
    bool established = false;
    while (std::chrono::steady_clock::now() < deadline && !established) {
        while (true) {
            quiche_send_info si = {};
            ssize_t w = quiche_conn_send(qc, out, sizeof(out), &si);
            if (w == QUICHE_ERR_DONE) break;
            if (w < 0) break;
            sendto(s, reinterpret_cast<const char*>(out),
                   static_cast<int>(w), 0,
                   reinterpret_cast<const sockaddr*>(&si.to),
                   static_cast<int>(si.to_len));
        }
        fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
        timeval tv = {0, 100 * 1000};
        if (::select(0, &fds, nullptr, nullptr, &tv) > 0) {
            sockaddr_in from = {}; int from_len = sizeof(from);
            int n = ::recvfrom(s, reinterpret_cast<char*>(in_buf),
                                sizeof(in_buf), 0,
                                reinterpret_cast<sockaddr*>(&from),
                                &from_len);
            if (n > 0) {
                quiche_recv_info ri = {
                    reinterpret_cast<sockaddr*>(&from), from_len,
                    reinterpret_cast<sockaddr*>(&cli),
                    static_cast<socklen_t>(cli_len),
                };
                quiche_conn_recv(qc, in_buf, n, &ri);
            }
        } else {
            quiche_conn_on_timeout(qc);
        }
        if (quiche_conn_is_established(qc)) established = true;
    }
    CHECK(established, "client failed to handshake against our self-gen cert");
    std::printf("[client] handshake established with self-gen cert\n");

    quiche_conn_free(qc);
    quiche_config_free(ccfg);
    closesocket(s);
    server.Shutdown();
    std::printf("[debug] keeping cert at %s\n", cert_path.c_str());
    std::printf("[debug] keeping key  at %s\n", key_path.c_str());
    // (intentionally leak temp files for ad-hoc inspection)
    ReleaseSelfSignedCert(cert);
    ++passed;
}

}  // namespace

int main() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    test_pem_round_trip_with_real_handshake();
    WSACleanup();
    std::printf("cert_util_pem_smoke_test: %d passed, %d failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
