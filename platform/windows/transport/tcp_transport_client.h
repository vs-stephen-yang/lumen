#pragma once

#include "lumen/transport/transport_client.h"
#include "lumen/transport/transport_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace lumen {

/// TCP-based transport client using Winsock2.
///
/// Connect() creates a TCP socket, connects to the server, and wraps the
/// result in a TcpTransportConnection. Supports automatic reconnection
/// with exponential backoff.
class TcpTransportClient : public TransportClient {
public:
    TcpTransportClient();
    ~TcpTransportClient() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Connect(ClientConnectedCallback callback) override;
    void SetReconnectPolicy(const ReconnectPolicy& policy) override;
    void Shutdown() override;

private:
    void ConnectThreadFunc(ClientConnectedCallback callback);
    Result<SOCKET> CreateAndConnect();

    TransportConfig config_;
    ReconnectPolicy reconnect_policy_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_requested_{false};

    std::mutex mutex_;
    std::thread connect_thread_;
    std::shared_ptr<TransportConnection> active_connection_;
};

}  // namespace lumen
