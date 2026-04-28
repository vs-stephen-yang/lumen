#pragma once

#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lumen {

class WsTransportConnection;

/// WebSocket transport server (no TLS yet — `ws://`, not `wss://`).
///
/// Listens on a TCP port. For each accepted socket, reads the HTTP/1.1
/// upgrade request, validates it, sends a 101 Switching Protocols
/// response, and wraps the socket in a WsTransportConnection that is
/// passed to the application via the IncomingConnectionCallback.
class WsTransportServer : public TransportServer {
public:
    WsTransportServer();
    ~WsTransportServer() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Start(IncomingConnectionCallback callback) override;
    void Stop() override;
    void Shutdown() override;
    std::string GetListenAddress() const override;
    uint16_t GetListenPort() const override;

private:
    void AcceptLoop(IncomingConnectionCallback callback);

    TransportConfig config_;
    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    uint16_t bound_port_ = 0;

    std::thread accept_thread_;
    mutable std::mutex mu_;
    std::vector<std::shared_ptr<TransportConnection>> connections_;
};

}  // namespace lumen
