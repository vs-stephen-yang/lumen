// web_receiver.exe — Windows receiver for the web sender.
//
// Phase 1 wires up: SignalingServer (HTTP), WsTransportServer (WebSocket
// over TCP), MfVideoDecoder, D3D11SwapChainRenderer (or count-only mode).
//
// S0 placeholder: just prints "ok" so the target builds and exists for
// later steps to fill in.

#include <cstdio>

int main(int /*argc*/, char** /*argv*/) {
    std::printf("web_receiver scaffolding ok\n");
    return 0;
}
