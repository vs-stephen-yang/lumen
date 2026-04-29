#pragma once

// HTTP/3 server skeleton — Phase 2.0 of the WebTransport pivot.
//
// This file owns the QUIC layer (msquic listener with ALPN "h3") and the
// per-connection bookkeeping. Future phases:
//   - 2.1: nghttp3 wiring (uni control stream, QPACK encoder/decoder)
//   - 2.2: HTTP/3 request/response framing
//   - 3.x: WebTransport extension on top
//
// Only the QUIC-level skeleton is implemented today. nghttp3 is included
// just so we can validate the link path; HTTP/3 events are not yet wired
// to its callbacks.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msquic.h>

#include "common/cert_util.h"

#include "lumen/common/error.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lumen {

class Http3Connection;

/// Configuration for the HTTP/3 server.
struct Http3ServerConfig {
    std::string address = "0.0.0.0";
    uint16_t port = 0;                // 0 = ephemeral

    // Cert
    std::wstring cert_key_name = L"LumenHttp3DevKey";
    std::wstring cert_subject_cn = L"lumen-receiver";
    int cert_validity_days = 14;      // WebTransport spec cap

    // Connection settings
    uint32_t idle_timeout_ms = 30000;
    uint32_t keep_alive_ms = 5000;
    uint32_t peer_bidi_streams = 100;
    uint32_t peer_unidi_streams = 100;
};

/// Callback fired when a new HTTP/3 connection is established (TLS done).
using Http3ConnectionCallback =
    std::function<void(std::shared_ptr<Http3Connection>)>;

/// Minimum HTTP/3 server. Generates its own ECDSA P-256 cert on Initialize
/// and exposes the SHA-256 fingerprint via GetCertSha256Hex() so the
/// signaling layer can hand it to the browser for serverCertificateHashes.
class Http3Server {
public:
    Http3Server();
    ~Http3Server();

    /// Open msquic, generate the cert, configure TLS. Does NOT yet bind
    /// the listener — call Start() for that.
    Result<void> Initialize(const Http3ServerConfig& config);

    /// Begin listening. The callback is invoked for each established
    /// connection (TLS handshake done).
    Result<void> Start(Http3ConnectionCallback callback);

    /// Stop the listener. Existing connections continue.
    void Stop();

    /// Tear everything down.
    void Shutdown();

    /// The actual port the listener is bound to (useful when port=0).
    uint16_t GetListenPort() const { return bound_port_; }

    /// Lowercase hex of SHA-256(DER cert). The receiver advertises this
    /// in /api/session and the browser passes it as
    /// `new WebTransport(url, { serverCertificateHashes: [...] })`.
    std::string GetCertSha256Hex() const;

private:
    static QUIC_STATUS QUIC_API ListenerCallback(
        HQUIC listener, void* context, QUIC_LISTENER_EVENT* event);
    QUIC_STATUS OnNewConnection(HQUIC connection,
                                 const QUIC_NEW_CONNECTION_INFO* info);

    Http3ServerConfig config_;
    const QUIC_API_TABLE* msquic_ = nullptr;
    HQUIC registration_ = nullptr;
    HQUIC configuration_ = nullptr;
    HQUIC listener_ = nullptr;
    SelfSignedCert cert_ = {};
    uint16_t bound_port_ = 0;
    std::atomic<bool> initialized_{false};

    Http3ConnectionCallback connection_callback_;

    mutable std::mutex mu_;
    std::vector<std::shared_ptr<Http3Connection>> connections_;
};

}  // namespace lumen
