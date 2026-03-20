#pragma once

#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <msquic.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lumen {

/// QUIC-based transport server using msquic.
///
/// Runs a QUIC listener that accepts incoming connections, wrapping each in a
/// QuicTransportConnection. TLS is mandatory (QUIC requires it). On Windows,
/// provide a PCCERT_CONTEXT via SetCertificateContext() before Start().
class QuicTransportServer : public TransportServer {
public:
    QuicTransportServer();
    ~QuicTransportServer() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Start(IncomingConnectionCallback callback) override;
    void Stop() override;
    void Shutdown() override;
    std::string GetListenAddress() const override;
    uint16_t GetListenPort() const override;

    /// Set the server TLS certificate (Windows PCCERT_CONTEXT).
    /// Must be called after Initialize() and before Start().
    /// The caller retains ownership of the certificate context.
    void SetCertificateContext(PCCERT_CONTEXT cert_context);

    /// Set the server TLS certificate by SHA1 hash (thumbprint).
    /// The cert must be in the CurrentUser\MY store.
    void SetCertificateHash(const QUIC_CERTIFICATE_HASH& cert_hash);

private:
    static QUIC_STATUS QUIC_API ListenerCallback(
        HQUIC listener, void* context, QUIC_LISTENER_EVENT* event);

    QUIC_STATUS OnNewConnection(HQUIC connection,
                                 const QUIC_NEW_CONNECTION_INFO* info);

    TransportConfig config_;
    const QUIC_API_TABLE* msquic_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    PCCERT_CONTEXT cert_context_ = nullptr;  // Non-owning
    QUIC_CERTIFICATE_HASH cert_hash_ = {};
    QUIC_CERTIFICATE_HASH_STORE cert_hash_store_ = {};
    bool use_cert_hash_ = false;
    bool use_cert_hash_store_ = false;
    uint16_t bound_port_ = 0;
    std::atomic<bool> initialized_{false};

    IncomingConnectionCallback incoming_callback_;

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<TransportConnection>> connections_;
};

}  // namespace lumen
