#include "quiche_server.h"

#include "wt_varint.h"

#include <quiche.h>

#include <bcrypt.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace lumen {

namespace {

constexpr size_t kMaxDatagramSize = 1350;
constexpr size_t kLocalConnIdLen  = 16;

bool RandomBytes(uint8_t* buf, size_t len) {
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string ToKey(const uint8_t* p, size_t n) {
    return std::string(reinterpret_cast<const char*>(p), n);
}

}  // namespace

struct QuicheServer::Connection {
    std::vector<uint8_t> scid;          // 16 random bytes we use to identify this conn
    quiche_conn* qc = nullptr;
    quiche_h3_conn* h3 = nullptr;       // lazily constructed once is_established
    sockaddr_storage peer = {};
    socklen_t peer_len = 0;
    std::chrono::steady_clock::time_point next_timeout =
        (std::chrono::steady_clock::time_point::max)();

    // Established WebTransport session ids (= stream id of the CONNECT
    // stream). One QUIC connection can host multiple sessions in
    // principle, though typical apps use one.
    std::unordered_set<uint64_t> wt_sessions;

    // Latest stats snapshot, refreshed on the recv thread (FlushEgress) and
    // read under QuicheServer::stats_mu_ by GetConnectionStats.
    TransportStats stats;

    ~Connection() {
        if (h3) quiche_h3_conn_free(h3);
        if (qc) quiche_conn_free(qc);
    }
};

namespace {

// Captures the request pseudo-headers we care about during the
// quiche_h3_event_for_each_header walk.
struct RequestHeaders {
    std::string method;
    std::string protocol;
    std::string scheme;
    std::string authority;
    std::string path;
};

int CaptureHeader(uint8_t* name, size_t name_len,
                   uint8_t* value, size_t value_len, void* arg) {
    auto* h = static_cast<RequestHeaders*>(arg);
    std::string n(reinterpret_cast<char*>(name), name_len);
    std::string v(reinterpret_cast<char*>(value), value_len);
    if      (n == ":method")    h->method    = std::move(v);
    else if (n == ":protocol")  h->protocol  = std::move(v);
    else if (n == ":scheme")    h->scheme    = std::move(v);
    else if (n == ":authority") h->authority = std::move(v);
    else if (n == ":path")      h->path      = std::move(v);
    return 0;
}

}  // namespace

QuicheServer::QuicheServer() = default;

QuicheServer::~QuicheServer() { Shutdown(); }

Result<void> QuicheServer::Initialize(const QuicheServerConfig& cfg) {
    user_cfg_ = cfg;

    quiche_cfg_ = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!quiche_cfg_) {
        return Error::Make(ErrorCode::kTransportError,
                            "quiche_config_new failed");
    }

    if (quiche_config_load_cert_chain_from_pem_file(
            quiche_cfg_, cfg.cert_pem_path.c_str()) != 0) {
        return Error::Make(ErrorCode::kTransportTlsError,
                            "load_cert_chain_from_pem_file failed: " +
                                cfg.cert_pem_path);
    }
    if (quiche_config_load_priv_key_from_pem_file(
            quiche_cfg_, cfg.key_pem_path.c_str()) != 0) {
        return Error::Make(ErrorCode::kTransportTlsError,
                            "load_priv_key_from_pem_file failed: " +
                                cfg.key_pem_path);
    }

    if (quiche_config_set_application_protos(
            quiche_cfg_, cfg.alpn.data(), cfg.alpn.size()) != 0) {
        return Error::Make(ErrorCode::kTransportError,
                            "set_application_protos failed");
    }

    quiche_config_set_max_idle_timeout(quiche_cfg_, cfg.max_idle_timeout_ms);
    quiche_config_set_max_recv_udp_payload_size(quiche_cfg_, kMaxDatagramSize);
    quiche_config_set_max_send_udp_payload_size(quiche_cfg_, kMaxDatagramSize);
    quiche_config_set_initial_max_data(quiche_cfg_, cfg.initial_max_data);
    quiche_config_set_initial_max_streams_bidi(quiche_cfg_,
                                                 cfg.initial_max_streams_bidi);
    quiche_config_set_initial_max_streams_uni(quiche_cfg_,
                                                cfg.initial_max_streams_uni);
    quiche_config_set_initial_max_stream_data_bidi_local(
        quiche_cfg_, cfg.initial_max_stream_data_bidi_local);
    quiche_config_set_initial_max_stream_data_bidi_remote(
        quiche_cfg_, cfg.initial_max_stream_data_bidi_remote);
    quiche_config_set_initial_max_stream_data_uni(
        quiche_cfg_, cfg.initial_max_stream_data_uni);
    quiche_config_set_cc_algorithm(quiche_cfg_, QUICHE_CC_CUBIC);
    if (cfg.enable_dgram) {
        quiche_config_enable_dgram(quiche_cfg_, true,
                                     cfg.dgram_recv_queue_len,
                                     cfg.dgram_send_queue_len);
    }

    if (cfg.enable_h3) {
        h3_cfg_ = quiche_h3_config_new();
        if (!h3_cfg_) {
            return Error::Make(ErrorCode::kTransportError,
                                "quiche_h3_config_new failed");
        }
        quiche_h3_config_enable_extended_connect(
            h3_cfg_, cfg.enable_h3_extended_connect);

        // Advertise SETTINGS_WT_MAX_SESSIONS = 1 so Chrome accepts the
        // WebTransport session. quiche itself doesn't know about this
        // setting; we inject it via the additional_settings hook added
        // in our quiche fork (see third_party/quiche/quiche/src/h3/ffi.rs).
        if (cfg.enable_h3_extended_connect) {
            // Different Chrome versions key off different draft setting
            // IDs. Send all three known values so the server is
            // accepted regardless of which draft Chrome's quiche tracks:
            //
            //   0x2b603742 — SETTINGS_WEBTRANS_DRAFT00 (oldest, Chrome
            //                google/quiche http_constants.h)
            //   0xc671706a — SETTINGS_WEBTRANS_MAX_SESSIONS_DRAFT07
            //                (Chrome google/quiche)
            //   0x14e9cd29 — WT_MAX_SESSIONS per draft-ietf-webtrans-
            //                http3-14 §9.2 IANA registration
            const uint64_t pairs[] = {
                0x2b603742ULL, 1ULL,
                0xc671706aULL, 1ULL,
                0x14e9cd29ULL, 1ULL,
            };
            (void)quiche_h3_config_set_additional_settings(
                h3_cfg_, pairs, sizeof(pairs) / sizeof(pairs[0]));
        }
    }

    return {};
}

Result<void> QuicheServer::Start() {
    if (!quiche_cfg_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }
    if (running_.exchange(true)) {
        return Error::Make(ErrorCode::kAlreadyInitialized);
    }

    sock_ = MakeUdpSocket();
    if (auto r = sock_->Open(); !r) {
        sock_.reset();
        running_.store(false);
        return r.error();
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(user_cfg_.port);
    inet_pton(AF_INET, user_cfg_.address.c_str(), &addr.sin_addr);

    if (auto r = sock_->Bind(reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        !r) {
        sock_.reset();
        running_.store(false);
        return r.error();
    }

    if (sock_->GetLocalAddr(local_addr_, local_addr_len_).ok()) {
        bound_port_ =
            ntohs(reinterpret_cast<sockaddr_in*>(&local_addr_)->sin_port);
    } else {
        bound_port_ = user_cfg_.port;
    }

    recv_thread_ = std::thread(&QuicheServer::RecvLoop, this);
    return {};
}

void QuicheServer::Stop() {
    if (!running_.exchange(false)) return;
    if (sock_) sock_->Close();  // unblocks WaitReadable in the recv loop
    if (recv_thread_.joinable()) recv_thread_.join();
}

void QuicheServer::Shutdown() {
    Stop();
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        conns_.clear();  // ~Connection frees quiche_conn
    }
    if (h3_cfg_) {
        quiche_h3_config_free(h3_cfg_);
        h3_cfg_ = nullptr;
    }
    if (quiche_cfg_) {
        quiche_config_free(quiche_cfg_);
        quiche_cfg_ = nullptr;
    }
}

size_t QuicheServer::GetConnectionCount() const {
    std::lock_guard<std::mutex> lock(conns_mu_);
    return conns_.size();
}

size_t QuicheServer::GetWebTransportSessionCount() const {
    std::lock_guard<std::mutex> lock(conns_mu_);
    size_t total = 0;
    for (const auto& [_, c] : conns_) total += c->wt_sessions.size();
    return total;
}

void QuicheServer::SetOnWebTransportSession(WebTransportSessionCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    wt_callback_ = std::move(cb);
}

void QuicheServer::SetOnWebTransportDatagram(
    WebTransportDatagramCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    dgram_callback_ = std::move(cb);
}

Result<void> QuicheServer::SendWebTransportDatagram(
    const std::string& conn_id, uint64_t session_id,
    const uint8_t* data, size_t size) {

    // Verify the connection exists. The conns_ map is only mutated on the
    // recv thread under conns_mu_, so this lookup is safe from any thread.
    // The session and the actual quiche_conn write happen on the recv thread
    // (DrainSendQueue) — we never touch quiche_conn from here.
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        if (conns_.find(conn_id) == conns_.end()) {
            return Error::Make(ErrorCode::kTransportNotConnected,
                                "no such conn_id");
        }
    }

    uint8_t prefix[8];
    const size_t prefix_len = VarintEncode(session_id, prefix, sizeof(prefix));
    if (prefix_len == 0) {
        return Error::Make(ErrorCode::kInvalidArgument, "session_id too big");
    }

    PendingSend ps;
    ps.conn_id = conn_id;
    ps.session_id = session_id;
    ps.payload.resize(prefix_len + size);
    std::memcpy(ps.payload.data(), prefix, prefix_len);
    if (size) std::memcpy(ps.payload.data() + prefix_len, data, size);

    {
        std::lock_guard<std::mutex> lock(send_mu_);
        send_q_.push_back(std::move(ps));
    }
    return {};
}

void QuicheServer::DrainSendQueue() {
    std::vector<PendingSend> batch;
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        if (send_q_.empty()) return;
        batch.swap(send_q_);
    }

    std::unordered_set<Connection*> touched;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        for (auto& ps : batch) {
            auto it = conns_.find(ps.conn_id);
            if (it == conns_.end()) continue;            // connection gone
            Connection* c = it->second.get();
            if (c->wt_sessions.count(ps.session_id) == 0) continue;  // no session
            (void)quiche_conn_dgram_send(c->qc, ps.payload.data(),
                                          ps.payload.size());
            touched.insert(c);
        }
    }
    // Flush outside conns_mu_ (same pattern as OnTimers). Single-threaded with
    // the rest of the recv loop, so the pointers remain valid.
    for (Connection* c : touched) FlushEgress(c);
}

void QuicheServer::UpdateStats(Connection* conn) {
    quiche_stats cs;
    std::memset(&cs, 0, sizeof(cs));
    quiche_conn_stats(conn->qc, &cs);

    quiche_path_stats ps;
    std::memset(&ps, 0, sizeof(ps));
    const bool have_path = quiche_conn_path_stats(conn->qc, 0, &ps) == 0;

    TransportStats t;
    t.bytes_sent = cs.sent_bytes;
    t.bytes_received = cs.recv_bytes;
    if (cs.sent > 0) {
        t.loss_rate =
            static_cast<double>(cs.lost) / static_cast<double>(cs.sent);
    }
    if (have_path) {
        t.rtt_us = ps.rtt / 1000;            // ns → µs
        t.rtt_variance_us = ps.rttvar / 1000;
        t.bandwidth_estimate_bps = ps.delivery_rate * 8;  // bytes/s → bits/s
        t.congestion_window = ps.cwnd;
    }

    std::lock_guard<std::mutex> lock(stats_mu_);
    conn->stats = t;
}

Result<void> QuicheServer::GetConnectionStats(const std::string& conn_id,
                                               TransportStats& out) const {
    std::lock_guard<std::mutex> lock(conns_mu_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) {
        return Error::Make(ErrorCode::kTransportNotConnected, "no such conn_id");
    }
    std::lock_guard<std::mutex> slock(stats_mu_);
    out = it->second->stats;
    return {};
}

void QuicheServer::RecvLoop() {
    std::vector<uint8_t> buf(65535);

    while (running_.load()) {
        // Short readiness wait so we can also fire connection timers and drain
        // queued sends between packets.
        if (sock_->WaitReadable(50)) {
            sockaddr_storage peer = {};
            socklen_t peer_len = sizeof(peer);
            int64_t n = sock_->Recv(buf.data(), buf.size(), peer, peer_len);
            if (n < 0) {
                if (!running_.load()) break;
            } else if (n > 0) {
                HandlePacket(buf.data(), static_cast<size_t>(n), peer,
                             peer_len);
            }
        }

        DrainSendQueue();
        OnTimers();
        GcClosed();
    }
}

void QuicheServer::HandlePacket(const uint8_t* data, size_t size,
                                  const sockaddr_storage& peer,
                                  socklen_t peer_len) {
    uint8_t  type = 0;
    uint32_t version = 0;
    uint8_t  scid[QUICHE_MAX_CONN_ID_LEN]; size_t scid_len = sizeof(scid);
    uint8_t  dcid[QUICHE_MAX_CONN_ID_LEN]; size_t dcid_len = sizeof(dcid);
    uint8_t  token[256];                   size_t token_len = sizeof(token);

    int rc = quiche_header_info(data, size, kLocalConnIdLen, &version, &type,
                                  scid, &scid_len, dcid, &dcid_len,
                                  token, &token_len);
    if (rc < 0) return;

    Connection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        auto it = conns_.find(ToKey(dcid, dcid_len));
        if (it != conns_.end()) conn = it->second.get();
    }

    if (!conn) {
        // No existing connection. Only INITIAL packets create new ones.
        if (!quiche_version_is_supported(version)) {
            // Skip version negotiation for this minimal server.
            return;
        }

        // Skip stateless retry for the smoke test (we trust localhost
        // peers). Real deployments should issue a Retry with a token.
        uint8_t our_scid[kLocalConnIdLen];
        if (!RandomBytes(our_scid, kLocalConnIdLen)) return;

        quiche_conn* qc = quiche_accept(
            our_scid, kLocalConnIdLen,
            /*odcid=*/nullptr, 0,
            reinterpret_cast<const sockaddr*>(&local_addr_), local_addr_len_,
            reinterpret_cast<const sockaddr*>(&peer),       peer_len,
            quiche_cfg_);
        if (!qc) return;

        auto conn_ptr = std::make_unique<Connection>();
        conn_ptr->scid.assign(our_scid, our_scid + kLocalConnIdLen);
        conn_ptr->qc = qc;
        std::memcpy(&conn_ptr->peer, &peer, peer_len);
        conn_ptr->peer_len = peer_len;

        Connection* raw = conn_ptr.get();
        {
            std::lock_guard<std::mutex> lock(conns_mu_);
            conns_.emplace(ToKey(our_scid, kLocalConnIdLen),
                           std::move(conn_ptr));
        }
        conn = raw;
    }

    quiche_recv_info recv_info = {
        const_cast<sockaddr*>(reinterpret_cast<const sockaddr*>(&peer)),
        peer_len,
        reinterpret_cast<sockaddr*>(&local_addr_),
        local_addr_len_,
    };
    (void)quiche_conn_recv(conn->qc, const_cast<uint8_t*>(data), size,
                            &recv_info);

    if (h3_cfg_ && quiche_conn_is_established(conn->qc)) {
        DriveHttp3(conn);
    }

    DrainDatagrams(conn);

    FlushEgress(conn);
}

void QuicheServer::DrainDatagrams(Connection* conn) {
    uint8_t buf[1500];
    while (true) {
        ssize_t n = quiche_conn_dgram_recv(conn->qc, buf, sizeof(buf));
        if (n <= 0) break;

        uint64_t sid = 0;
        const size_t consumed = VarintDecode(buf, static_cast<size_t>(n), &sid);
        if (consumed == 0) continue;          // malformed
        if (conn->wt_sessions.count(sid) == 0) continue;  // unknown session

        WebTransportDatagramCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mu_);
            cb = dgram_callback_;
        }
        if (cb) {
            cb(std::string(reinterpret_cast<const char*>(conn->scid.data()),
                            conn->scid.size()),
               sid, buf + consumed,
               static_cast<size_t>(n) - consumed);
        }
    }
}

void QuicheServer::DriveHttp3(Connection* conn) {
    if (!conn->h3) {
        conn->h3 = quiche_h3_conn_new_with_transport(conn->qc, h3_cfg_);
        if (!conn->h3) return;
    }

    while (true) {
        quiche_h3_event* ev = nullptr;
        int64_t s = quiche_h3_conn_poll(conn->h3, conn->qc, &ev);
        if (s < 0) break;

        switch (quiche_h3_event_type(ev)) {
            case QUICHE_H3_EVENT_HEADERS: {
                RequestHeaders req;
                quiche_h3_event_for_each_header(ev, &CaptureHeader, &req);

                const bool is_wt_connect =
                    req.method == "CONNECT" && req.protocol == "webtransport";

                if (is_wt_connect) {
                    // RFC 9220 §3.3: respond 200, fin=false. The CONNECT
                    // stream stays open as the session stream.
                    static const uint8_t kStatus[]    = ":status";
                    static const uint8_t kStatusVal[] = "200";
                    quiche_h3_header headers[] = {
                        {kStatus,    sizeof(kStatus)    - 1, kStatusVal,
                                      sizeof(kStatusVal) - 1},
                    };
                    quiche_h3_send_response(conn->h3, conn->qc, s, headers,
                                              1, /*fin=*/false);
                    conn->wt_sessions.insert(static_cast<uint64_t>(s));

                    WebTransportSessionCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(cb_mu_);
                        cb = wt_callback_;
                    }
                    if (cb) {
                        WebTransportSessionInfo info;
                        info.conn_id.assign(
                            reinterpret_cast<const char*>(conn->scid.data()),
                            conn->scid.size());
                        info.session_id = static_cast<uint64_t>(s);
                        info.authority  = std::move(req.authority);
                        info.path       = std::move(req.path);
                        cb(info);
                    }
                } else {
                    // Plain HTTP/3 request — Phase 2.2 stub reply.
                    static const uint8_t kStatus[]    = ":status";
                    static const uint8_t kStatusVal[] = "200";
                    static const uint8_t kServer[]    = "server";
                    static const uint8_t kServerVal[] = "lumen-quiche";
                    static const uint8_t kCl[]        = "content-length";
                    static const uint8_t kClVal[]     = "3";
                    quiche_h3_header headers[] = {
                        {kStatus,    sizeof(kStatus)    - 1, kStatusVal,
                                      sizeof(kStatusVal) - 1},
                        {kServer,    sizeof(kServer)    - 1, kServerVal,
                                      sizeof(kServerVal) - 1},
                        {kCl,        sizeof(kCl)        - 1, kClVal,
                                      sizeof(kClVal)     - 1},
                    };
                    quiche_h3_send_response(conn->h3, conn->qc, s, headers,
                                              3, false);
                    static const uint8_t kBody[] = "ok\n";
                    quiche_h3_send_body(conn->h3, conn->qc, s,
                                         const_cast<uint8_t*>(kBody),
                                         sizeof(kBody) - 1, true);
                }
                break;
            }
            case QUICHE_H3_EVENT_DATA:
            case QUICHE_H3_EVENT_FINISHED:
            case QUICHE_H3_EVENT_RESET:
            case QUICHE_H3_EVENT_PRIORITY_UPDATE:
            case QUICHE_H3_EVENT_GOAWAY:
                break;
        }
        quiche_h3_event_free(ev);
    }
}

void QuicheServer::FlushEgress(Connection* conn) {
    uint8_t out[kMaxDatagramSize];
    while (true) {
        quiche_send_info si = {};
        ssize_t written = quiche_conn_send(conn->qc, out, sizeof(out), &si);
        if (written == QUICHE_ERR_DONE) break;
        if (written < 0) return;

        sock_->SendTo(out, static_cast<size_t>(written),
                      reinterpret_cast<const sockaddr*>(&si.to), si.to_len);
    }

    // Schedule the next timer for this connection.
    uint64_t ns = quiche_conn_timeout_as_nanos(conn->qc);
    if (ns == UINT64_MAX) {
        conn->next_timeout = (std::chrono::steady_clock::time_point::max)();
    } else {
        conn->next_timeout = std::chrono::steady_clock::now() +
                             std::chrono::nanoseconds(ns);
    }

    // Refresh the cached stats snapshot (recv-thread only).
    UpdateStats(conn);
}

void QuicheServer::OnTimers() {
    auto now = std::chrono::steady_clock::now();
    std::vector<Connection*> to_flush;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        for (auto& [k, c] : conns_) {
            if (c->next_timeout != (std::chrono::steady_clock::time_point::max)() &&
                c->next_timeout <= now) {
                quiche_conn_on_timeout(c->qc);
                to_flush.push_back(c.get());
            }
        }
    }
    for (auto* c : to_flush) FlushEgress(c);
}

void QuicheServer::GcClosed() {
    std::lock_guard<std::mutex> lock(conns_mu_);
    for (auto it = conns_.begin(); it != conns_.end();) {
        if (quiche_conn_is_closed(it->second->qc)) {
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace lumen
