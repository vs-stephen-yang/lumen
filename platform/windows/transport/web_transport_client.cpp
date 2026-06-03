#include "web_transport_client.h"

#include "quiche_client.h"
#include "web_transport_client_connection.h"

#include <utility>

namespace lumen {

WebTransportClient::WebTransportClient()
    : client_(std::make_unique<QuicheClient>()) {}

WebTransportClient::~WebTransportClient() { Shutdown(); }

Result<void> WebTransportClient::Initialize(const TransportConfig& config) {
    config_ = config;

    QuicheClientConfig ccfg;
    if (!config.address.empty()) ccfg.host = config.address;
    ccfg.port        = config.port;
    ccfg.verify_peer = config.tls.verify_peer;
    // TransportConfig has no WT path field; default to the receiver's mount.
    ccfg.path        = "/lumen";
    ccfg.authority   = "localhost";

    return client_->Initialize(ccfg);
}

Result<void> WebTransportClient::Connect(ClientConnectedCallback callback) {
    connected_cb_ = std::move(callback);

    const std::string remote =
        config_.address + ":" + std::to_string(config_.port);

    client_->SetOnSession([this, remote](uint64_t session_id) {
        auto conn = std::make_shared<WebTransportClientConnection>(
            client_.get(), session_id, remote);
        {
            std::lock_guard<std::mutex> lock(mu_);
            conn_ = conn;
        }
        if (connected_cb_) connected_cb_(conn);
    });

    client_->SetOnDatagram(
        [this](uint64_t /*session_id*/, const uint8_t* data, size_t size) {
            std::shared_ptr<WebTransportClientConnection> conn;
            {
                std::lock_guard<std::mutex> lock(mu_);
                conn = conn_;
            }
            if (conn) conn->OnDatagram(data, size);
        });

    return client_->Start();
}

void WebTransportClient::SetReconnectPolicy(const ReconnectPolicy& policy) {
    reconnect_policy_ = policy;
}

void WebTransportClient::Shutdown() {
    if (client_) client_->Shutdown();
    std::lock_guard<std::mutex> lock(mu_);
    conn_.reset();
}

}  // namespace lumen
