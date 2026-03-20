#pragma once

#include "lumen/transport/transport_client.h"
#include "lumen/transport/transport_types.h"
#include "quic_transport_connection.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msquic.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace lumen {

/// QUIC-based transport client using msquic.
///
/// Connect() opens a QUIC connection to the server. The handshake and stream
/// setup happen asynchronously — the callback fires once the connection is
/// fully established (QUIC CONNECTED event + client streams opened).
class QuicTransportClient : public TransportClient {
public:
    QuicTransportClient();
    ~QuicTransportClient() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Connect(ClientConnectedCallback callback) override;
    void SetReconnectPolicy(const ReconnectPolicy& policy) override;
    void Shutdown() override;

private:
    TransportConfig config_;
    const QUIC_API_TABLE* msquic_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutdown_requested_{false};
    ReconnectPolicy reconnect_policy_;

    mutable std::mutex mutex_;
    std::shared_ptr<QuicTransportConnection> active_connection_;
};

}  // namespace lumen
