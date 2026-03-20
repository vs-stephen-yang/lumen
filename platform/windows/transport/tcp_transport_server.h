#pragma once

#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lumen {

/// TCP-based transport server using Winsock2.
///
/// Runs an accept loop on a background thread, wrapping each accepted
/// socket in a TcpTransportConnection and firing the incoming callback.
class TcpTransportServer : public TransportServer {
public:
    TcpTransportServer();
    ~TcpTransportServer() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Start(IncomingConnectionCallback callback) override;
    void Stop() override;
    void Shutdown() override;
    std::string GetListenAddress() const override;
    uint16_t GetListenPort() const override;

private:
    void AcceptThreadFunc(IncomingConnectionCallback callback);

    TransportConfig config_;
    SOCKET listen_socket_ = INVALID_SOCKET;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stop_requested_{false};
    uint16_t bound_port_ = 0;

    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<TransportConnection>> connections_;
};

}  // namespace lumen
