// web_receiver.exe — Lumen receiver for the web sender.
//
// Phase 2.5+: real WebCodecs encode → WebTransport → native decode pipeline.
//
//   - Generates an ECDSA P-256 self-signed cert at startup and writes
//     it as PEM to two files in %TEMP%. The cert SHA-256 is advertised
//     via /api/session so the browser pins it via
//     `new WebTransport(url, { serverCertificateHashes: [...] })`.
//
//   - QuicheServer hosts the WebTransport endpoint on udp/<wt-port>.
//
//   - Each WT datagram payload is `[channel_id:u8][MediaPacketHeader:28]
//     [fragment payload]`. Fragments are reassembled via FrameReassembler
//     (one per channel). Reassembled video frames are fed to
//     MfVideoDecoder; audio packets are counted (Opus decode is left
//     as a follow-up).
//
//   - Per-second FPS is logged. When the page sends a control-channel
//     `done:video=...,audio=...,dt_ms=...` message, the receiver emits
//     a `FINAL_STATS:` line that out-of-process tests assert on.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#pragma comment(lib, "mfplat.lib")

#include "lumen/signaling/signaling_server.h"
#include "lumen/signaling/session_config.h"
#include "common/cert_util.h"
#include "common/d3d11_device_context.h"
#include "transport/quiche_server.h"
#include "codec/mf_video_decoder.h"

#include "frame_reassembler.h"
#include "lumen/transport/transport_types.h"
#include "lumen/common/types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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

    // ── Cert + temp PEMs ────────────────────────────────────────────
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
        ReleaseSelfSignedCert(cert); return 1;
    }
    const std::string sha256_hex = Sha256ToHex(cert.sha256_der);

    // ── Media Foundation startup (required before any MFT use). ────
    if (FAILED(MFStartup(MF_VERSION))) {
        std::fprintf(stderr, "MFStartup failed\n");
        ReleaseSelfSignedCert(cert);
        return 1;
    }

    // ── D3D11 + decoder. Required so received H.264 chunks can be
    // ── fed to MfVideoDecoder for actual decode (not just counting).
    auto device_ctx = std::make_unique<D3D11DeviceContext>();
    bool decoder_ok = false;
    std::unique_ptr<MfVideoDecoder> decoder;
    if (auto r = device_ctx->Initialize(0); r.ok()) {
        decoder = std::make_unique<MfVideoDecoder>(*device_ctx);
        VideoDecoderConfig dcfg;
        dcfg.codec = VideoCodec::kH264;
        dcfg.width = 320;
        dcfg.height = 240;
        dcfg.fps = 30;
        dcfg.low_latency = true;
        if (auto d = decoder->Initialize(dcfg, device_ctx->Device()); d.ok()) {
            decoder_ok = true;
            std::printf("[recv] H.264 decoder ready (320x240@30)\n");
        } else {
            std::printf("[recv] H.264 decoder init failed: %s — counting "
                         "received frames only\n",
                         d.error().message.c_str());
        }
    } else {
        std::printf("[recv] D3D11 device init failed: %s — counting "
                     "received frames only\n", r.error().message.c_str());
    }

    // ── Reassemblers + stats ────────────────────────────────────────
    FrameReassembler video_ra;
    FrameReassembler audio_ra;
    FrameReassembler ctrl_ra;

    std::atomic<uint64_t> video_received{0};
    std::atomic<uint64_t> video_decoded{0};
    std::atomic<uint64_t> video_decode_errors{0};
    std::atomic<uint64_t> video_keyframes{0};
    std::atomic<uint64_t> video_bytes{0};
    std::atomic<uint64_t> audio_received{0};
    std::atomic<uint64_t> audio_bytes{0};
    auto t_start = std::chrono::steady_clock::now();
    auto t_first_video = std::chrono::steady_clock::time_point{};
    std::mutex first_mu;

    video_ra.SetCallback(
        [&](const uint8_t* data, size_t size, const MediaPacketHeader& h) {
            video_received.fetch_add(1);
            video_bytes.fetch_add(size);
            if (h.IsKeyframe()) video_keyframes.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(first_mu);
                if (t_first_video == std::chrono::steady_clock::time_point{}) {
                    t_first_video = std::chrono::steady_clock::now();
                }
            }
            if (decoder_ok) {
                auto r = decoder->Decode(data, size, h.frame_index);
                if (r.ok()) {
                    video_decoded.fetch_add(1);
                    decoder->ReleaseFrame(r.value());
                } else if (r.error().code != ErrorCode::kTimeout) {
                    video_decode_errors.fetch_add(1);
                }
            }
        });

    audio_ra.SetCallback(
        [&](const uint8_t* /*data*/, size_t size,
             const MediaPacketHeader& /*h*/) {
            audio_received.fetch_add(1);
            audio_bytes.fetch_add(size);
        });

    std::atomic<bool> got_done{false};
    std::string done_msg;
    std::mutex done_mu;
    ctrl_ra.SetCallback(
        [&](const uint8_t* data, size_t size, const MediaPacketHeader&) {
            std::string m(reinterpret_cast<const char*>(data), size);
            std::lock_guard<std::mutex> lock(done_mu);
            done_msg = std::move(m);
            got_done.store(true);
        });

    // ── QuicheServer ────────────────────────────────────────────────
    QuicheServer wt;
    QuicheServerConfig wcfg;
    wcfg.address = "0.0.0.0";
    wcfg.port    = wt_port;
    wcfg.cert_pem_path = cert_pem;
    wcfg.key_pem_path  = key_pem;
    if (auto r = wt.Initialize(wcfg); !r) {
        std::fprintf(stderr, "QuicheServer.Initialize failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    wt.SetOnWebTransportSession(
        [](const WebTransportSessionInfo& info) {
            std::printf("[wt] session established path=%s session_id=%llu\n",
                         info.path.c_str(),
                         (unsigned long long)info.session_id);
            std::fflush(stdout);
        });

    wt.SetOnWebTransportDatagram(
        [&](const std::string& /*conn_id*/, uint64_t /*session_id*/,
             const uint8_t* data, size_t size) {
            if (size < 1 + 28) return;
            const uint8_t channel = data[0];
            const uint8_t* body = data + 1;          // header + payload
            const size_t   body_size = size - 1;
            switch (channel) {
                case 0: video_ra.AddFragment(body, body_size); break;
                case 1: audio_ra.AddFragment(body, body_size); break;
                case 2: ctrl_ra.AddFragment(body, body_size); break;
                default: break;
            }
        });

    if (auto r = wt.Start(); !r) {
        std::fprintf(stderr, "QuicheServer.Start failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    // ── Signaling ───────────────────────────────────────────────────
    SessionConfig cfg;
    cfg.session_id = "demo";
    cfg.transport.kind = "webtransport";
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

    std::printf("\nLumen web_receiver — WebCodecs WebTransport pipeline\n");
    std::printf("  Signaling   : http://localhost:%u/sender/\n",
                 signal.GetPort());
    std::printf("  Session     : http://localhost:%u/api/session\n",
                 signal.GetPort());
    std::printf("  WebTransport: https://localhost:%u/lumen\n",
                 wt.GetListenPort());
    std::printf("  Cert SHA-256: %s\n", sha256_hex.c_str());
    std::printf("  Decoder     : %s\n",
                 decoder_ok ? "MfVideoDecoder H.264" : "(disabled)");
    std::printf("\nOpen the signaling URL in Chrome/Edge. "
                 "Press Ctrl-C to stop.\n\n");
    std::fflush(stdout);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    // ── Stats heartbeat ─────────────────────────────────────────────
    auto last_v_recv = uint64_t{0};
    auto last_v_dec  = uint64_t{0};
    auto last_a_recv = uint64_t{0};
    auto last_tick = std::chrono::steady_clock::now();
    while (!g_stop.load() && !got_done.load()) {
        std::this_thread::sleep_for(200ms);
        auto now = std::chrono::steady_clock::now();
        if (now - last_tick >= 1s) {
            const auto vr = video_received.load();
            const auto vd = video_decoded.load();
            const auto ar = audio_received.load();
            const double dt = std::chrono::duration<double>(now - last_tick).count();
            std::printf("[fps] video recv=%llu (+%.1f/s) decoded=%llu "
                         "(+%.1f/s) audio recv=%llu (+%.1f/s) keyframes=%llu\n",
                         (unsigned long long)vr,
                         (vr - last_v_recv) / dt,
                         (unsigned long long)vd,
                         (vd - last_v_dec) / dt,
                         (unsigned long long)ar,
                         (ar - last_a_recv) / dt,
                         (unsigned long long)video_keyframes.load());
            std::fflush(stdout);
            last_v_recv = vr; last_v_dec = vd; last_a_recv = ar;
            last_tick = now;
        }
    }

    // ── Final report ────────────────────────────────────────────────
    auto t_end = std::chrono::steady_clock::now();
    const double dt_total =
        std::chrono::duration<double>(t_end - t_start).count();
    const auto vr = video_received.load();
    const auto vd = video_decoded.load();
    const auto ar = audio_received.load();
    const auto ve = video_decode_errors.load();
    const auto kf = video_keyframes.load();
    const auto vb = video_bytes.load();
    const auto ab = audio_bytes.load();
    {
        std::lock_guard<std::mutex> lock(done_mu);
        std::printf("[recv] page reported: %s\n", done_msg.c_str());
    }
    std::printf("FINAL_STATS: dt_s=%.3f video_received=%llu video_decoded=%llu "
                 "video_decode_errors=%llu video_keyframes=%llu video_bytes=%llu "
                 "audio_received=%llu audio_bytes=%llu "
                 "video_recv_fps=%.2f video_decode_fps=%.2f audio_pkt_per_s=%.2f\n",
                 dt_total,
                 (unsigned long long)vr, (unsigned long long)vd,
                 (unsigned long long)ve, (unsigned long long)kf,
                 (unsigned long long)vb,
                 (unsigned long long)ar, (unsigned long long)ab,
                 vr / (dt_total > 0.001 ? dt_total : 0.001),
                 vd / (dt_total > 0.001 ? dt_total : 0.001),
                 ar / (dt_total > 0.001 ? dt_total : 0.001));
    std::fflush(stdout);

    wt.Shutdown();
    signal.Stop();
    if (decoder) decoder.reset();
    if (device_ctx) device_ctx.reset();
    MFShutdown();
    DeleteFileA(cert_pem.c_str());
    DeleteFileA(key_pem.c_str());
    ReleaseSelfSignedCert(cert);
    return 0;
}
