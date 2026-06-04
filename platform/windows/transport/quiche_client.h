#pragma once

// QUIC + HTTP/3 + WebTransport client built on quiche (Cloudflare).
//
// Mirrors QuicheServer for the client side: a single QUIC connection driven
// by one recv thread. quiche_connect performs the handshake, an HTTP/3
// extended CONNECT opens the WebTransport session, and media flows over WT
// datagrams. As on the server, only the recv thread touches quiche_conn —
// sends are enqueued and drained there, stats are snapshotted there.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "lumen/common/error.h"
#include "lumen/transport/transport_types.h"
#include "lumen/transport/udp_socket.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct quiche_config;
struct quiche_conn;
struct quiche_h3_config;
struct quiche_h3_conn;

namespace lumen {

struct QuicheClientConfig {
    std::string host = "127.0.0.1";   // server address (IPv4)
    uint16_t    port = 0;
    std::string authority = "localhost";  // :authority for the CONNECT
    std::string path = "/lumen";          // :path for the CONNECT

    std::vector<uint8_t> alpn = {0x02, 'h', '3'};
    bool verify_peer = false;             // self-signed servers by default

    uint64_t max_idle_timeout_ms = 30000;
    uint64_t initial_max_data    = 10'000'000;
    uint64_t initial_max_streams_bidi = 100;
    uint64_t initial_max_streams_uni  = 100;
    uint64_t initial_max_stream_data_bidi_local  = 1'000'000;
    uint64_t initial_max_stream_data_bidi_remote = 1'000'000;
    uint64_t initial_max_stream_data_uni         = 1'000'000;
    bool enable_dgram = true;
    uint64_t dgram_recv_queue_len = 1024;
    uint64_t dgram_send_queue_len = 1024;
};

class QuicheClient {
public:
    /// Fired on the recv thread once the WebTransport session is established.
    using SessionCallback = std::function<void(uint64_t session_id)>;
    /// Fired on the recv thread for each inbound WT datagram (session_id
    /// varint already stripped).
    using DatagramCallback =
        std::function<void(uint64_t session_id, const uint8_t* data,
                           size_t size)>;

    QuicheClient();
    ~QuicheClient();

    Result<void> Initialize(const QuicheClientConfig& cfg);
    /// Open the socket, start the QUIC handshake, and spin the recv thread.
    Result<void> Start();
    void Stop();
    void Shutdown();

    void SetOnSession(SessionCallback cb);
    void SetOnDatagram(DatagramCallback cb);

    /// Queue a WT datagram (thread-safe). The session_id varint is prepended;
    /// the actual quiche write happens on the recv thread.
    Result<void> SendWebTransportDatagram(uint64_t session_id,
                                          const uint8_t* data, size_t size);

    /// Latest cached transport stats (lock-guarded snapshot, recv-thread only).
    Result<void> GetConnectionStats(TransportStats& out) const;

    bool SessionEstablished() const { return session_established_.load(); }

private:
    void RecvLoop();
    void MaybeSendConnect();
    void DriveHttp3();
    void DrainDatagrams();
    void DrainSendQueue();
    void FlushEgress();
    void UpdateStats();

    QuicheClientConfig cfg_;
    quiche_config*    quiche_cfg_ = nullptr;
    quiche_h3_config* h3_cfg_     = nullptr;
    quiche_conn*      qc_         = nullptr;
    quiche_h3_conn*   h3_         = nullptr;

    std::unique_ptr<UdpSocket> sock_;
    sockaddr_storage local_addr_ = {};
    socklen_t        local_addr_len_ = 0;
    sockaddr_storage peer_addr_ = {};
    socklen_t        peer_addr_len_ = 0;

    std::thread recv_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> session_established_{false};
    bool      sent_connect_ = false;
    int64_t   connect_stream_ = -1;
    uint64_t  session_id_ = 0;

    mutable std::mutex cb_mu_;
    SessionCallback  session_cb_;
    DatagramCallback dgram_cb_;

    mutable std::mutex send_mu_;
    std::vector<std::vector<uint8_t>> send_q_;  // each: [varint(session)][app]

    mutable std::mutex stats_mu_;
    TransportStats stats_;
};

}  // namespace lumen
