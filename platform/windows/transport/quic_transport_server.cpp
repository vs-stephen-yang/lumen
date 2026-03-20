#include "quic_transport_server.h"
#include "quic_transport_connection.h"

#include <algorithm>

namespace lumen {

static const QUIC_BUFFER kAlpn = {
    sizeof("lumen") - 1, const_cast<uint8_t*>(
                              reinterpret_cast<const uint8_t*>("lumen"))};

QuicTransportServer::QuicTransportServer() = default;

QuicTransportServer::~QuicTransportServer() {
    Shutdown();
}

Result<void> QuicTransportServer::Initialize(const TransportConfig& config) {
    if (QUIC_FAILED(MsQuicOpen2(&msquic_))) {
        return Error::Make(ErrorCode::kTransportError, "MsQuicOpen2 failed");
    }

    const QUIC_REGISTRATION_CONFIG reg_config = {
        "lumen-server", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(msquic_->RegistrationOpen(&reg_config, &registration_))) {
        MsQuicClose(msquic_);
        msquic_ = nullptr;
        return Error::Make(ErrorCode::kTransportError,
                          "RegistrationOpen failed");
    }

    // Configure QUIC settings.
    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs = 30000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 4;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerUnidiStreamCount = 0;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
    settings.IsSet.ServerResumptionLevel = TRUE;
    settings.KeepAliveIntervalMs = 5000;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    settings.CongestionControlAlgorithm = QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC;
    settings.IsSet.CongestionControlAlgorithm = TRUE;

    if (QUIC_FAILED(msquic_->ConfigurationOpen(
            registration_, &kAlpn, 1, &settings, sizeof(settings), nullptr,
            &configuration_))) {
        msquic_->RegistrationClose(registration_);
        MsQuicClose(msquic_);
        msquic_ = nullptr;
        return Error::Make(ErrorCode::kTransportError,
                          "ConfigurationOpen failed");
    }

    config_ = config;
    initialized_ = true;
    return {};
}

void QuicTransportServer::SetCertificateContext(PCCERT_CONTEXT cert_context) {
    cert_context_ = cert_context;
    use_cert_hash_ = false;
}

void QuicTransportServer::SetCertificateHash(
    const QUIC_CERTIFICATE_HASH& cert_hash) {
    cert_hash_ = cert_hash;
    use_cert_hash_ = true;
    use_cert_hash_store_ = false;

    // Also set up hash-store variant for CurrentUser\MY.
    memcpy(cert_hash_store_.ShaHash, cert_hash.ShaHash, 20);
    strncpy(cert_hash_store_.StoreName, "MY",
            sizeof(cert_hash_store_.StoreName));
    cert_hash_store_.Flags = QUIC_CERTIFICATE_HASH_STORE_FLAG_NONE;
    use_cert_hash_store_ = true;
}

Result<void> QuicTransportServer::Start(IncomingConnectionCallback callback) {
    if (!initialized_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    // Load TLS credentials.
    QUIC_CREDENTIAL_CONFIG cred_config = {};
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_NONE |
                        QUIC_CREDENTIAL_FLAG_SET_ALLOWED_CIPHER_SUITES;
    cred_config.AllowedCipherSuites =
        QUIC_ALLOWED_CIPHER_SUITE_AES_128_GCM_SHA256 |
        QUIC_ALLOWED_CIPHER_SUITE_AES_256_GCM_SHA384;

    if (use_cert_hash_store_) {
        cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
        cred_config.CertificateHashStore = &cert_hash_store_;
    } else if (use_cert_hash_) {
        cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
        cred_config.CertificateHash = &cert_hash_;
    } else if (cert_context_) {
        cred_config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
        cred_config.CertificateContext =
            const_cast<CERT_CONTEXT*>(cert_context_);
    } else {
        return Error::Make(ErrorCode::kTransportTlsError,
                          "No server certificate provided");
    }

    QUIC_STATUS cred_status = msquic_->ConfigurationLoadCredential(
        configuration_, &cred_config);
    if (QUIC_FAILED(cred_status)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ConfigurationLoadCredential failed: 0x%08X",
                 static_cast<unsigned>(cred_status));
        return Error::Make(ErrorCode::kTransportTlsError, buf);
    }

    // Open and start the listener.
    if (QUIC_FAILED(msquic_->ListenerOpen(
            registration_, ListenerCallback, this, &listener_))) {
        return Error::Make(ErrorCode::kTransportError,
                          "ListenerOpen failed");
    }

    QUIC_ADDR address = {};
    QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&address, config_.port);

    if (QUIC_FAILED(msquic_->ListenerStart(listener_, &kAlpn, 1, &address))) {
        msquic_->ListenerClose(listener_);
        listener_ = nullptr;
        return Error::Make(ErrorCode::kTransportError,
                          "ListenerStart failed");
    }

    // Retrieve actual bound port.
    QUIC_ADDR bound_addr = {};
    uint32_t addr_len = sizeof(bound_addr);
    if (QUIC_SUCCEEDED(msquic_->GetParam(
            listener_, QUIC_PARAM_LISTENER_LOCAL_ADDRESS, &addr_len,
            &bound_addr))) {
        bound_port_ = QuicAddrGetPort(&bound_addr);
    }

    incoming_callback_ = std::move(callback);
    return {};
}

void QuicTransportServer::Stop() {
    if (listener_) {
        msquic_->ListenerClose(listener_);
        listener_ = nullptr;
    }
}

void QuicTransportServer::Shutdown() {
    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& conn : connections_) {
            conn->Close();
        }
        connections_.clear();
    }

    if (configuration_) {
        msquic_->ConfigurationClose(configuration_);
        configuration_ = nullptr;
    }
    if (registration_) {
        msquic_->RegistrationClose(registration_);
        registration_ = nullptr;
    }
    if (msquic_) {
        MsQuicClose(msquic_);
        msquic_ = nullptr;
    }

    initialized_ = false;
}

std::string QuicTransportServer::GetListenAddress() const {
    return config_.address.empty() ? "0.0.0.0" : config_.address;
}

uint16_t QuicTransportServer::GetListenPort() const {
    return bound_port_;
}

// ---------------------------------------------------------------------------
// msquic listener callback
// ---------------------------------------------------------------------------

QUIC_STATUS QUIC_API QuicTransportServer::ListenerCallback(
    HQUIC listener, void* context, QUIC_LISTENER_EVENT* event) {
    auto* self = static_cast<QuicTransportServer*>(context);

    switch (event->Type) {
    case QUIC_LISTENER_EVENT_NEW_CONNECTION:
        return self->OnNewConnection(
            event->NEW_CONNECTION.Connection,
            event->NEW_CONNECTION.Info);
    case QUIC_LISTENER_EVENT_STOP_COMPLETE:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QuicTransportServer::OnNewConnection(
    HQUIC connection, const QUIC_NEW_CONNECTION_INFO* info) {
    // Format remote address.
    char addr_buf[64] = {};
    if (info && info->RemoteAddress) {
        QUIC_ADDR_STR addr_str = {};
        QuicAddrToString(info->RemoteAddress, &addr_str);
        strncpy(addr_buf, addr_str.Address, sizeof(addr_buf) - 1);
    }

    auto conn = std::make_shared<QuicTransportConnection>(
        msquic_, true, std::string(addr_buf));
    conn->SetHandle(connection);

    // Set callback handler (must be done before returning).
    msquic_->SetCallbackHandler(
        connection,
        reinterpret_cast<void*>(QuicTransportConnection::ConnectionCallback),
        conn.get());

    // Provide TLS configuration (must be done before returning).
    QUIC_STATUS status =
        msquic_->ConnectionSetConfiguration(connection, configuration_);
    if (QUIC_FAILED(status)) return status;

    // Wire up the connect callback to fire our incoming callback.
    conn->SetConnectCallback(
        [this](std::shared_ptr<TransportConnection> c) {
            if (incoming_callback_) {
                incoming_callback_(c);
            }
        });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Sweep dead connections.
        connections_.erase(
            std::remove_if(connections_.begin(), connections_.end(),
                [](const auto& c) {
                    auto s = c->GetState();
                    return s == ConnectionState::kDisconnected ||
                           s == ConnectionState::kFailed;
                }),
            connections_.end());
        connections_.push_back(conn);
    }

    return QUIC_STATUS_SUCCESS;
}

}  // namespace lumen
