#include "web_transport_server.h"

#include "quiche_server.h"
#include "web_transport_connection.h"

#include <utility>

namespace lumen {

WebTransportServer::WebTransportServer()
    : server_(std::make_unique<QuicheServer>()) {}

WebTransportServer::~WebTransportServer() { Shutdown(); }

std::string WebTransportServer::MakeKey(const std::string& conn_id,
                                         uint64_t session_id) {
    return conn_id + "#" + std::to_string(session_id);
}

Result<void> WebTransportServer::Initialize(const TransportConfig& config) {
    config_ = config;

    QuicheServerConfig wcfg;
    if (!config.address.empty()) wcfg.address = config.address;
    wcfg.port          = config.port;
    wcfg.cert_pem_path = config.tls.cert_path;
    wcfg.key_pem_path  = config.tls.key_path;
    // ALPN ("h3"), HTTP/3, extended CONNECT and datagrams keep their
    // QuicheServerConfig defaults — those are what browsers negotiate.

    return server_->Initialize(wcfg);
}

Result<void> WebTransportServer::Start(IncomingConnectionCallback callback) {
    incoming_callback_ = std::move(callback);

    server_->SetOnWebTransportSession(
        [this](const WebTransportSessionInfo& info) { OnSession(info); });
    server_->SetOnWebTransportDatagram(
        [this](const std::string& conn_id, uint64_t session_id,
               const uint8_t* data, size_t size) {
            OnDatagram(conn_id, session_id, data, size);
        });

    return server_->Start();
}

void WebTransportServer::OnSession(const WebTransportSessionInfo& info) {
    auto conn = std::make_shared<WebTransportConnection>(
        server_.get(), info.conn_id, info.session_id, info.authority);

    {
        std::lock_guard<std::mutex> lock(mu_);
        conns_.emplace(MakeKey(info.conn_id, info.session_id), conn);
    }

    if (incoming_callback_) incoming_callback_(conn);
}

void WebTransportServer::OnDatagram(const std::string& conn_id,
                                     uint64_t session_id, const uint8_t* data,
                                     size_t size) {
    std::shared_ptr<WebTransportConnection> conn;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = conns_.find(MakeKey(conn_id, session_id));
        if (it != conns_.end()) conn = it->second;
    }
    // Release the lock before dispatching: the receive path may decode
    // synchronously and must not block other sessions' setup.
    if (conn) conn->OnDatagram(data, size);
}

void WebTransportServer::Stop() {
    if (server_) server_->Stop();
}

void WebTransportServer::Shutdown() {
    if (server_) server_->Shutdown();
    std::lock_guard<std::mutex> lock(mu_);
    conns_.clear();
}

std::string WebTransportServer::GetListenAddress() const {
    return config_.address;
}

uint16_t WebTransportServer::GetListenPort() const {
    return server_ ? server_->GetListenPort() : 0;
}

}  // namespace lumen
