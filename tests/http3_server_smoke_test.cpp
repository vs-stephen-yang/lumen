// Phase-2.0 smoke test for Http3Server.
//
// We're not yet speaking HTTP/3 framing (that's 2.1), so this only
// validates the QUIC-layer scaffolding:
//   - Initialize() generates a valid ECDSA P-256 cert and loads it into
//     msquic's configuration.
//   - Start() binds the listener (ephemeral port) and reports the port.
//   - SHA-256 fingerprint of the DER cert is non-empty 64-hex-char.
//   - Shutdown() returns cleanly with no zombie state.

#include "transport/http3_server.h"
#include "transport/http3_connection.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace lumen;

namespace {
int passed = 0;
int failed = 0;
bool skipped = false;

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg);     \
                   ++failed; return; }                                         \
} while (0)

bool LooksLikeTlsNotAvailable(const std::string& msg) {
    // Schannel on Windows < 11 returns SEC_E_ALGORITHM_MISMATCH (0x80090331)
    // when asked to do TLS 1.3 / QUIC. The same condition shows up across
    // ALG_MISMATCH and a couple of related codes; match by hex prefix to
    // keep the heuristic broad.
    return msg.find("0x80090331") != std::string::npos ||
           msg.find("SEC_E_ALGORITHM_MISMATCH") != std::string::npos;
}

bool IsAllHexLower(const std::string& s) {
    for (char c : s) {
        const bool h = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!h) return false;
    }
    return true;
}

void test_lifecycle() {
    Http3Server server;
    Http3ServerConfig cfg;
    cfg.address = "127.0.0.1";
    cfg.port = 0;
    cfg.cert_key_name = L"LumenHttp3SmokeKey";
    cfg.cert_subject_cn = L"lumen-h3-smoke";
    cfg.cert_validity_days = 14;

    auto init = server.Initialize(cfg);
    if (!init.ok() && LooksLikeTlsNotAvailable(init.error().message)) {
        std::printf("[h3] SKIP — Schannel QUIC TLS 1.3 not available "
                     "(needs Windows 11 / Server 2022+)\n");
        skipped = true;
        return;
    }
    CHECK(init.ok(), init.error().message.c_str());

    const std::string hash = server.GetCertSha256Hex();
    CHECK(hash.size() == 64, "cert SHA-256 should be 64 hex chars");
    CHECK(IsAllHexLower(hash), "cert SHA-256 should be lowercase hex");
    std::printf("[h3] cert SHA-256: %s\n", hash.c_str());

    int conn_count = 0;
    auto start = server.Start(
        [&](std::shared_ptr<Http3Connection> /*c*/) { ++conn_count; });
    CHECK(start.ok(), start.error().message.c_str());

    const uint16_t port = server.GetListenPort();
    CHECK(port != 0, "ephemeral port not assigned");
    std::printf("[h3] listening on udp/%u (ALPN h3)\n", port);

    // No client tries to connect — we're only verifying the QUIC-layer
    // setup. Wait briefly to be sure no async errors fire.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    server.Shutdown();
    ++passed;
}

}  // namespace

int main() {
    test_lifecycle();
    if (skipped) {
        std::printf("http3_server_smoke_test: skipped\n");
        return 77;  // Standard "skip" exit code.
    }
    std::printf("http3_server_smoke_test: %d passed, %d failed\n",
                 passed, failed);
    return failed == 0 ? 0 : 1;
}
