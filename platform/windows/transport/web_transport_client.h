#pragma once

#include "lumen/transport/transport_client.h"
#include "lumen/transport/transport_types.h"

#include <memory>
#include <mutex>

namespace lumen {

class QuicheClient;
class WebTransportClientConnection;

/// WebTransport-based transport client.
///
/// Wraps a QuicheClient (QUIC + HTTP/3 + WebTransport over BoringSSL) behind
/// the abstract TransportClient interface, for native->native QUIC. Connect()
/// performs the handshake + extended CONNECT; once the WebTransport session is
/// established the ClientConnectedCallback fires with a TransportConnection
/// exposing video/audio/control channels.
class WebTransportClient : public TransportClient {
public:
    WebTransportClient();
    ~WebTransportClient() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Connect(ClientConnectedCallback callback) override;
    void SetReconnectPolicy(const ReconnectPolicy& policy) override;
    void Shutdown() override;

private:
    std::unique_ptr<QuicheClient> client_;
    TransportConfig config_;
    ReconnectPolicy reconnect_policy_;
    ClientConnectedCallback connected_cb_;

    mutable std::mutex mu_;
    std::shared_ptr<WebTransportClientConnection> conn_;
};

}  // namespace lumen
