// web_receiver.exe — Lumen receiver for the web sender.
//
// Phase 1 entry point. Currently runs the connectivity test:
//   - SignalingServer on http://localhost:<signal-port>
//       /sender/  → static test page (test_page/index.html)
//       /api/session → JSON pointing at the WS URL
//   - WsTransportServer on ws://localhost:<ws-port>
//       Logs each incoming connection and the bytes that arrive on each
//       channel.
//
// Decode + render are wired in a later step (S7); for now we simply
// observe that browsers can reach a native WS server hosted on the same
// machine.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "lumen/signaling/signaling_server.h"
#include "lumen/signaling/session_config.h"
#include "transport/ws_transport_server.h"

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

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

const char* ChannelName(ChannelType t) {
    switch (t) {
        case ChannelType::kVideo:   return "video";
        case ChannelType::kAudio:   return "audio";
        case ChannelType::kControl: return "control";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t signal_port = 8080;
    uint16_t ws_port = 8443;
    std::string web_root = "test_page";

    for (int i = 1; i < argc; ++i) {
        std::string val;
        if (ParseArg(argv[i], "--signal-port=", val)) {
            signal_port = static_cast<uint16_t>(std::atoi(val.c_str()));
        } else if (ParseArg(argv[i], "--ws-port=", val)) {
            ws_port = static_cast<uint16_t>(std::atoi(val.c_str()));
        } else if (ParseArg(argv[i], "--web-root=", val)) {
            web_root = std::move(val);
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            std::fprintf(stderr,
                "usage: web_receiver [--signal-port=N] [--ws-port=N] "
                "[--web-root=PATH]\n");
            return 2;
        }
    }

    SessionConfig cfg;
    cfg.session_id = "demo";
    cfg.transport.kind = "websocket";
    cfg.transport.url = "ws://localhost:" + std::to_string(ws_port) + "/lumen";
    cfg.transport.ssrc = 0xCAFEBEEF;

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

    WsTransportServer ws;
    TransportConfig tc;
    tc.address = "0.0.0.0";
    tc.port = ws_port;
    if (auto r = ws.Initialize(tc); !r) {
        std::fprintf(stderr, "ws.Initialize failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    std::atomic<int> conn_seq{0};
    auto on_conn = [&](std::shared_ptr<TransportConnection> conn) {
        const int id = conn_seq.fetch_add(1) + 1;
        std::printf("[ws] connection #%d from %s\n",
                    id, conn->GetRemoteAddress().c_str());
        std::fflush(stdout);

        for (auto type : {ChannelType::kVideo, ChannelType::kAudio,
                          ChannelType::kControl}) {
            auto* ch = conn->GetChannel(type);
            if (!ch) continue;
            ch->SetReceiveCallback(
                [id, type](const uint8_t* data, size_t size) {
                    std::printf("[ws #%d] %s: %zu bytes — first byte 0x%02X\n",
                                id, ChannelName(type), size,
                                size > 0 ? data[0] : 0);
                    std::fflush(stdout);
                });
        }
    };
    if (auto r = ws.Start(on_conn); !r) {
        std::fprintf(stderr, "ws.Start failed: %s\n",
                     r.error().message.c_str());
        return 1;
    }

    std::printf("\n");
    std::printf("Lumen web_receiver — connectivity test\n");
    std::printf("  Signaling : http://localhost:%u/sender/\n",
                signal.GetPort());
    std::printf("  Session   : http://localhost:%u/api/session\n",
                signal.GetPort());
    std::printf("  WebSocket : ws://localhost:%u/lumen\n",
                ws.GetListenPort());
    std::printf("  Web root  : %s\n", web_root.c_str());
    std::printf("\nOpen the signaling URL in Chrome/Edge.\n");
    std::printf("Press Ctrl-C to stop.\n\n");
    std::fflush(stdout);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    while (!g_stop.load()) std::this_thread::sleep_for(200ms);

    std::printf("Shutting down …\n");
    ws.Shutdown();
    signal.Stop();
    return 0;
}
