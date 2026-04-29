#include "http3_server.h"

#include "http3_connection.h"

#include <algorithm>
#include <cstdio>

namespace lumen {

namespace {

// HTTP/3 ALPN per RFC 9114.
const QUIC_BUFFER kAlpn = {
    static_cast<uint32_t>(sizeof("h3") - 1),
    const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>("h3"))};

}  // namespace

Http3Server::Http3Server() = default;

Http3Server::~Http3Server() { Shutdown(); }

Result<void> Http3Server::Initialize(const Http3ServerConfig& config) {
    config_ = config;

    if (QUIC_FAILED(MsQuicOpen2(&msquic_))) {
        return Error::Make(ErrorCode::kTransportError, "MsQuicOpen2 failed");
    }

    const QUIC_REGISTRATION_CONFIG reg_config = {
        "lumen-h3", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (QUIC_FAILED(msquic_->RegistrationOpen(&reg_config, &registration_))) {
        MsQuicClose(msquic_);
        msquic_ = nullptr;
        return Error::Make(ErrorCode::kTransportError,
                            "RegistrationOpen failed");
    }

    QUIC_SETTINGS settings = {};
    settings.IdleTimeoutMs = config.idle_timeout_ms;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = static_cast<uint16_t>(
        std::min<uint32_t>(config.peer_bidi_streams, 0xFFFF));
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerUnidiStreamCount = static_cast<uint16_t>(
        std::min<uint32_t>(config.peer_unidi_streams, 0xFFFF));
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;
    settings.KeepAliveIntervalMs = config.keep_alive_ms;
    settings.IsSet.KeepAliveIntervalMs = TRUE;
    settings.ServerResumptionLevel = QUIC_SERVER_NO_RESUME;
    settings.IsSet.ServerResumptionLevel = TRUE;
    settings.CongestionControlAlgorithm =
        QUIC_CONGESTION_CONTROL_ALGORITHM_CUBIC;
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

    // Generate the self-signed cert and load it into the configuration.
    cert_ = CreateSelfSignedCert(config.cert_key_name, config.cert_subject_cn,
                                  config.cert_validity_days);
    if (!cert_.valid) {
        return Error::Make(ErrorCode::kTransportTlsError,
                            "self-signed cert creation failed");
    }

    QUIC_CERTIFICATE_HASH_STORE hash_store = {};
    std::memcpy(hash_store.ShaHash, cert_.sha1_thumbprint.ShaHash, 20);
    std::memcpy(hash_store.StoreName, "MY", 3);
    hash_store.Flags = QUIC_CERTIFICATE_HASH_STORE_FLAG_NONE;

    QUIC_CREDENTIAL_CONFIG cred = {};
    cred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH_STORE;
    cred.CertificateHashStore = &hash_store;
    cred.Flags = QUIC_CREDENTIAL_FLAG_SET_ALLOWED_CIPHER_SUITES;
    cred.AllowedCipherSuites =
        QUIC_ALLOWED_CIPHER_SUITE_AES_128_GCM_SHA256 |
        QUIC_ALLOWED_CIPHER_SUITE_AES_256_GCM_SHA384;

    QUIC_STATUS load_status =
        msquic_->ConfigurationLoadCredential(configuration_, &cred);
    if (QUIC_FAILED(load_status)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                       "ConfigurationLoadCredential failed: 0x%08X",
                       static_cast<unsigned>(load_status));
        return Error::Make(ErrorCode::kTransportTlsError, buf);
    }

    initialized_.store(true);
    return {};
}

Result<void> Http3Server::Start(Http3ConnectionCallback callback) {
    if (!initialized_.load()) {
        return Error::Make(ErrorCode::kNotInitialized);
    }
    connection_callback_ = std::move(callback);

    if (QUIC_FAILED(msquic_->ListenerOpen(registration_, ListenerCallback,
                                            this, &listener_))) {
        return Error::Make(ErrorCode::kTransportError, "ListenerOpen failed");
    }

    QUIC_ADDR addr = {};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&addr, config_.port);

    if (QUIC_FAILED(msquic_->ListenerStart(listener_, &kAlpn, 1, &addr))) {
        msquic_->ListenerClose(listener_);
        listener_ = nullptr;
        return Error::Make(ErrorCode::kTransportError, "ListenerStart failed");
    }

    QUIC_ADDR bound = {};
    uint32_t bound_len = sizeof(bound);
    if (QUIC_SUCCEEDED(msquic_->GetParam(listener_,
                                           QUIC_PARAM_LISTENER_LOCAL_ADDRESS,
                                           &bound_len, &bound))) {
        bound_port_ = QuicAddrGetPort(&bound);
    } else {
        bound_port_ = config_.port;
    }
    return {};
}

void Http3Server::Stop() {
    if (listener_) {
        msquic_->ListenerClose(listener_);
        listener_ = nullptr;
    }
}

void Http3Server::Shutdown() {
    Stop();

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& c : connections_) c->Close();
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

    if (cert_.valid) {
        ReleaseSelfSignedCert(cert_);
    }

    initialized_.store(false);
}

std::string Http3Server::GetCertSha256Hex() const {
    if (!cert_.valid) return {};
    return Sha256ToHex(cert_.sha256_der);
}

QUIC_STATUS QUIC_API Http3Server::ListenerCallback(
    HQUIC /*listener*/, void* context, QUIC_LISTENER_EVENT* event) {
    auto* self = static_cast<Http3Server*>(context);
    switch (event->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION:
            return self->OnNewConnection(event->NEW_CONNECTION.Connection,
                                          event->NEW_CONNECTION.Info);
        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS Http3Server::OnNewConnection(
    HQUIC connection, const QUIC_NEW_CONNECTION_INFO* info) {
    char addr_buf[64] = {};
    if (info && info->RemoteAddress) {
        QUIC_ADDR_STR addr_str = {};
        QuicAddrToString(info->RemoteAddress, &addr_str);
        std::strncpy(addr_buf, addr_str.Address, sizeof(addr_buf) - 1);
    }

    auto conn = std::make_shared<Http3Connection>(msquic_, connection,
                                                    std::string(addr_buf));
    conn->AttachCallback();

    QUIC_STATUS status =
        msquic_->ConnectionSetConfiguration(connection, configuration_);
    if (QUIC_FAILED(status)) return status;

    {
        std::lock_guard<std::mutex> lock(mu_);
        connections_.push_back(conn);
    }
    if (connection_callback_) connection_callback_(conn);
    return QUIC_STATUS_SUCCESS;
}

}  // namespace lumen
