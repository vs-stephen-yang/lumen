// web_receiver.exe — Lumen receiver for the web sender.
//
// Phase 2.5 wiring: real WebTransport receiver.
//
//   - Generates an ECDSA P-256 self-signed cert at startup and writes
//     it as PEM to two files in %TEMP%. The cert's SHA-256 fingerprint
//     is advertised via /api/session so the browser can pin it via
//     `new WebTransport(url, { serverCertificateHashes: [...] })`.
//
//   - QuicheServer hosts the WebTransport endpoint on udp/<wt-port>.
//     Each established session is logged; incoming WT datagrams are
//     echoed back with an "echo: " prefix so the browser test page
//     can verify the round-trip.
//
//   - SignalingServer hosts /api/session + the static test page.
//
// (The legacy WebSocket path is left in tree for the moment but is no
//  longer started by this binary — it can be re-enabled by passing
//  --legacy-ws-port=N.)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "lumen/signaling/signaling_server.h"
#include "lumen/signaling/session_config.h"
#include "common/cert_util.h"
#include "transport/quiche_server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace lumen;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI ConsoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

bool ParseArg(const char* arg, const char* prefix, std::string& out) {
    const size_t n = std::strlen(prefix);
    if (std::strncmp(arg, prefix, n) == 0) {
        out = std::string(arg + n);
        return true;
    }
    return false;
}

std::string TempFile(const char* tag) {
    char dir[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    char name[MAX_PATH];
    GetTempFileNameA(dir, tag, 0, name);
    return std::string(name);
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t signal_port = 8080;
    uint16_t wt_port     = 8443;
    std::string web_root = "test_page";

    for (int i = 1; i < argc; ++i) {
        std::string val;
        if (ParseArg(argv[i], "--signal-port=", val)) {
            signal_port = static_cast<uint16_t>(std::atoi(val.c_str()));
        } else if (ParseArg(argv[i], "--wt-port=", val)) {
            wt_port = static_cast<uint16_t>(std::atoi(val.c_str()));
        } else if (ParseArg(argv[i], "--web-root=", val)) {
            web_root = std::move(val);
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            std::fprintf(stderr,
                "usage: web_receiver [--signal-port=N] [--wt-port=N] "
                "[--web-root=PATH]\n");
            return 2;
        }
    }

    // ── Generate cert + write PEMs ──────────────────────────────────
    // Chrome's serverCertificateHashes requires validity ≤ 14 days. Pass
    // 13 to leave margin against rounding / clock skew.
    auto cert = CreateSelfSignedCert(L"LumenWebReceiverKey",
                                       L"lumen-receiver", 13);
    if (!cert.valid) {
        std::fprintf(stderr, "cert generation failed\n");
        return 1;
    }
    const std::string cert_pem = TempFile("lcrt");
    const std::string key_pem  = TempFile("lkey");
    if (!WriteCertAndKeyPemFiles(cert, cert_pem, key_pem)) {
        std::fprintf(stderr, "WriteCertAndKeyPemFiles failed\n");
        ReleaseSelfSignedCert(cert);
        return 1;
    }
    const std::string sha256_hex = Sha256ToHex(cert.sha256_der);

    // ── Start QuicheServer (WebTransport) ───────────────────────────
    QuicheServer wt;
    QuicheServerConfig wt_cfg;
    wt_cfg.address = "0.0.0.0";
    wt_cfg.port    = wt_port;
    wt_cfg.cert_pem_path = cert_pem;
    wt_cfg.key_pem_path  = key_pem;
    if (auto r = wt.Initialize(wt_cfg); !r) {
        std::fprintf(stderr, "QuicheServer.Initialize failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    std::atomic<int> sessions_seen{0};
    wt.SetOnWebTransportSession(
        [&](const WebTransportSessionInfo& info) {
            ++sessions_seen;
            std::printf("[wt] session #%d established path=%s session_id=%llu\n",
                         sessions_seen.load(), info.path.c_str(),
                         (unsigned long long)info.session_id);
            std::fflush(stdout);
        });

    wt.SetOnWebTransportDatagram(
        [&](const std::string& conn_id, uint64_t session_id,
             const uint8_t* data, size_t size) {
            std::printf("[wt] dgram on session %llu, %zu bytes: \"%.*s\"\n",
                         (unsigned long long)session_id, size,
                         static_cast<int>(size), data);
            std::fflush(stdout);

            // Echo the bytes back with an "echo: " prefix so the test
            // page can verify the round-trip.
            std::string out = "echo: ";
            out.append(reinterpret_cast<const char*>(data), size);
            wt.SendWebTransportDatagram(conn_id, session_id,
                                          reinterpret_cast<const uint8_t*>(out.data()),
                                          out.size());
        });

    if (auto r = wt.Start(); !r) {
        std::fprintf(stderr, "QuicheServer.Start failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    // ── Start SignalingServer with cert hash in the JSON ────────────
    SessionConfig cfg;
    cfg.session_id = "demo";
    cfg.transport.kind = "webtransport";
    // Use 127.0.0.1 explicitly so Chrome doesn't pick IPv6 ::1 (the
    // QuicheServer currently binds IPv4 only).
    cfg.transport.url = "https://127.0.0.1:" + std::to_string(wt_port) +
                        "/lumen";
    cfg.transport.ssrc = 0xCAFEBEEF;
    cfg.transport.cert_sha256 = sha256_hex;

    SignalingServer signal;
    if (auto r = signal.Initialize(signal_port, web_root); !r) {
        std::fprintf(stderr, "signaling.Initialize failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }
    signal.SetSessionConfig(cfg);
    if (auto r = signal.Start(); !r) {
        std::fprintf(stderr, "signaling.Start failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    std::printf("\nLumen web_receiver — WebTransport\n");
    std::printf("  Signaling   : http://localhost:%u/sender/\n",
                 signal.GetPort());
    std::printf("  Session     : http://localhost:%u/api/session\n",
                 signal.GetPort());
    std::printf("  WebTransport: https://localhost:%u/lumen\n",
                 wt.GetListenPort());
    std::printf("  Cert SHA-256: %s\n", sha256_hex.c_str());
    std::printf("  Web root    : %s\n", web_root.c_str());
    std::printf("\nOpen the signaling URL in Chrome/Edge. Press Ctrl-C "
                 "to stop.\n\n");
    std::fflush(stdout);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    while (!g_stop.load()) std::this_thread::sleep_for(200ms);

    std::printf("Shutting down...\n");
    wt.Shutdown();
    signal.Stop();
    DeleteFileA(cert_pem.c_str());
    DeleteFileA(key_pem.c_str());
    ReleaseSelfSignedCert(cert);
    return 0;
}
