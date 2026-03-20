#include "quic_transport_client.h"

namespace lumen {

static const QUIC_BUFFER kAlpn = {
    sizeof("lumen") - 1, const_cast<uint8_t*>(
                              reinterpret_cast<const uint8_t*>("lumen"))};

QuicTransportClient::QuicTransportClient() = default;

QuicTransportClient::~QuicTransportClient() {
    Shutdown();
}

Result<void> QuicTransportClient::Initialize(const TransportConfig& config) {
    if (QUIC_FAILED(MsQuicOpen2(&msquic_))) {
        return Error::Make(ErrorCode::kTransportError, "MsQuicOpen2 failed");
    }

    const QUIC_REGISTRATION_CONFIG reg_config = {
        "lumen-client", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(msquic_->RegistrationOpen(&reg_config, &registration_))) {
        MsQuicClose(msquic_);
        msquic_ = nullptr;
        return Error::Make(ErrorCode::kTransportError,
                          "RegistrationOpen failed");
    }

    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs = 30000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
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

    // Client TLS: no client certificate, skip server cert validation for now.
    QUIC_CREDENTIAL_CONFIG cred_config = {};
    cred_config.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred_config.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                        QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

    if (QUIC_FAILED(msquic_->ConfigurationLoadCredential(
            configuration_, &cred_config))) {
        msquic_->ConfigurationClose(configuration_);
        msquic_->RegistrationClose(registration_);
        MsQuicClose(msquic_);
        msquic_ = nullptr;
        return Error::Make(ErrorCode::kTransportTlsError,
                          "Client ConfigurationLoadCredential failed");
    }

    config_ = config;
    initialized_ = true;
    return {};
}

Result<void> QuicTransportClient::Connect(ClientConnectedCallback callback) {
    if (!initialized_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    std::string remote = config_.address + ":" + std::to_string(config_.port);

    // Create our connection wrapper first (so we can pass it as context).
    auto conn = std::make_shared<QuicTransportConnection>(
        msquic_, false, remote);

    // Open the QUIC connection handle, with our static callback and the
    // connection wrapper as context.
    HQUIC quic_conn = nullptr;
    QUIC_STATUS status = msquic_->ConnectionOpen(
        registration_,
        QuicTransportConnection::ConnectionCallback,
        conn.get(),
        &quic_conn);
    if (QUIC_FAILED(status)) {
        return Error::Make(ErrorCode::kTransportConnectionFailed,
                          "ConnectionOpen failed");
    }

    conn->SetHandle(quic_conn);
    conn->SetConnectCallback(std::move(callback));

    // Store the shared_ptr to keep the connection alive.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connection_ = conn;
    }

    // Initiate the QUIC handshake (async — result comes via callback).
    status = msquic_->ConnectionStart(
        quic_conn, configuration_, QUIC_ADDRESS_FAMILY_UNSPEC,
        config_.address.c_str(), config_.port);
    if (QUIC_FAILED(status)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_connection_.reset();
        }
        msquic_->ConnectionClose(quic_conn);
        return Error::Make(ErrorCode::kTransportConnectionFailed,
                          "ConnectionStart failed");
    }

    return {};
}

void QuicTransportClient::SetReconnectPolicy(const ReconnectPolicy& policy) {
    reconnect_policy_ = policy;
}

void QuicTransportClient::Shutdown() {
    shutdown_requested_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_connection_) {
            active_connection_->Close();
            active_connection_.reset();
        }
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

}  // namespace lumen
