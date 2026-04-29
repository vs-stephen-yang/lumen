#pragma once

// QUIC server built on quiche (Cloudflare).
//
// Phase 2.1: UDP listener + quiche_accept routing. No HTTP/3 framing
// (Phase 2.2) and no WebTransport extension (Phase 2.3). Single recv
// thread that owns all connections; sends are issued from the same
// thread immediately after a recv that produced output.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "lumen/common/error.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct quiche_config;
struct quiche_conn;
struct quiche_h3_config;
struct quiche_h3_conn;

namespace lumen {

struct QuicheServerConfig {
    std::string address = "0.0.0.0";
    uint16_t    port    = 0;        // 0 = ephemeral

    // Server cert + key, PEM format.
    std::string cert_pem_path;
    std::string key_pem_path;

    // ALPN values, length-prefixed concatenated form per RFC 7301.
    // Default: "h3" for HTTP/3.
    std::vector<uint8_t> alpn = {0x02, 'h', '3'};

    uint64_t max_idle_timeout_ms = 30000;
    uint64_t initial_max_data    = 10'000'000;
    uint64_t initial_max_streams_bidi  = 100;
    uint64_t initial_max_streams_uni   = 100;
    uint64_t initial_max_stream_data_bidi_local  = 1'000'000;
    uint64_t initial_max_stream_data_bidi_remote = 1'000'000;
    uint64_t initial_max_stream_data_uni         = 1'000'000;
    bool enable_dgram = true;
    uint64_t dgram_recv_queue_len = 1024;
    uint64_t dgram_send_queue_len = 1024;

    // HTTP/3
    bool enable_h3 = true;
    bool enable_h3_extended_connect = true;  // required for WebTransport
};

/// Information about an established WebTransport session.
struct WebTransportSessionInfo {
    /// QUIC connection's primary SCID (string of bytes used as a stable id).
    std::string conn_id;
    /// HTTP/3 stream id that hosts the session (the CONNECT stream).
    uint64_t session_id = 0;
    /// :authority and :path from the CONNECT request, for routing.
    std::string authority;
    std::string path;
};

class QuicheServer {
public:
    using WebTransportSessionCallback =
        std::function<void(const WebTransportSessionInfo&)>;

    /// Fired on the recv thread for each incoming WebTransport datagram.
    /// `conn_id`/`session_id` identify the session; `data` is the
    /// payload after the session_id varint has been stripped.
    using WebTransportDatagramCallback = std::function<void(
        const std::string& conn_id, uint64_t session_id,
        const uint8_t* data, size_t size)>;

    QuicheServer();
    ~QuicheServer();

    /// Open winsock, configure quiche, load cert chain + private key.
    Result<void> Initialize(const QuicheServerConfig& cfg);

    /// Bind the UDP socket and spin the recv thread.
    Result<void> Start();

    /// Stop the recv thread and close the socket.
    void Stop();

    /// Tear everything down.
    void Shutdown();

    uint16_t GetListenPort() const { return bound_port_; }

    /// Number of QUIC connections currently tracked.
    size_t GetConnectionCount() const;

    /// Number of currently established WebTransport sessions.
    size_t GetWebTransportSessionCount() const;

    /// Set a callback fired (on the recv thread) for each WT session
    /// established via extended CONNECT. The callback must not block.
    void SetOnWebTransportSession(WebTransportSessionCallback cb);

    /// Set a callback fired for each incoming WT datagram on any session.
    void SetOnWebTransportDatagram(WebTransportDatagramCallback cb);

    /// Send a WebTransport datagram on the given session. The session_id
    /// varint is prepended automatically; the caller passes only the
    /// application payload.
    Result<void> SendWebTransportDatagram(const std::string& conn_id,
                                          uint64_t session_id,
                                          const uint8_t* data, size_t size);

private:
    struct Connection;

    void RecvLoop();
    void HandlePacket(const uint8_t* data, size_t size,
                      const sockaddr_storage& peer, socklen_t peer_len);
    void DriveHttp3(Connection* conn);
    void DrainDatagrams(Connection* conn);
    void FlushEgress(Connection* conn);
    void GcClosed();
    void OnTimers();

    QuicheServerConfig user_cfg_;
    quiche_config* quiche_cfg_ = nullptr;
    quiche_h3_config* h3_cfg_ = nullptr;

    mutable std::mutex cb_mu_;
    WebTransportSessionCallback wt_callback_;
    WebTransportDatagramCallback dgram_callback_;

    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_storage local_addr_ = {};
    socklen_t local_addr_len_ = 0;
    uint16_t bound_port_ = 0;

    std::thread recv_thread_;
    std::atomic<bool> running_{false};

    // Connection map keyed by SCID bytes (the DCID the peer uses to
    // address this connection).
    mutable std::mutex conns_mu_;
    std::unordered_map<std::string, std::unique_ptr<Connection>> conns_;
};

}  // namespace lumen
