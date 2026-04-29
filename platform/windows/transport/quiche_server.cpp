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

bool EnsureWsaStarted() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    return ok;
}

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
    if (!EnsureWsaStarted()) {
        return Error::Make(ErrorCode::kTransportError, "WSAStartup failed");
    }

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

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        running_.store(false);
        return Error::Make(ErrorCode::kTransportError, "socket() failed");
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(user_cfg_.port);
    inet_pton(AF_INET, user_cfg_.address.c_str(), &addr.sin_addr);

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr),
                 sizeof(addr)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        running_.store(false);
        return Error::Make(ErrorCode::kTransportError,
                            "bind() failed: " + std::to_string(err));
    }

    sockaddr_in actual = {};
    int actual_len = sizeof(actual);
    if (getsockname(sock_, reinterpret_cast<sockaddr*>(&actual),
                     &actual_len) == 0) {
        bound_port_ = ntohs(actual.sin_port);
        std::memcpy(&local_addr_, &actual, actual_len);
        local_addr_len_ = actual_len;
    } else {
        bound_port_ = user_cfg_.port;
    }

    recv_thread_ = std::thread(&QuicheServer::RecvLoop, this);
    return {};
}

void QuicheServer::Stop() {
    if (!running_.exchange(false)) return;
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
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

    Connection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        auto it = conns_.find(conn_id);
        if (it == conns_.end()) {
            return Error::Make(ErrorCode::kTransportNotConnected,
                                "no such conn_id");
        }
        if (it->second->wt_sessions.count(session_id) == 0) {
            return Error::Make(ErrorCode::kTransportNotConnected,
                                "session not established");
        }
        conn = it->second.get();
    }

    uint8_t prefix[8];
    const size_t prefix_len = VarintEncode(session_id, prefix, sizeof(prefix));
    if (prefix_len == 0) {
        return Error::Make(ErrorCode::kInvalidArgument, "session_id too big");
    }

    std::vector<uint8_t> buf(prefix_len + size);
    std::memcpy(buf.data(), prefix, prefix_len);
    if (size) std::memcpy(buf.data() + prefix_len, data, size);

    ssize_t rc = quiche_conn_dgram_send(conn->qc, buf.data(), buf.size());
    if (rc < 0) {
        return Error::Make(ErrorCode::kTransportError,
                            "quiche_conn_dgram_send rc=" +
                                std::to_string(rc));
    }
    // Egress will flush from the recv thread on the next iteration; we
    // also kick a flush here for snappier sends.
    FlushEgress(conn);
    return {};
}

void QuicheServer::RecvLoop() {
    std::vector<uint8_t> buf(65535);

    while (running_.load()) {
        // select() with a short timeout so we can also fire connection
        // timers between packets.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_, &fds);
        timeval tv = {0, 50 * 1000};  // 50ms
        int sel = ::select(0, &fds, nullptr, nullptr, &tv);
        if (sel == SOCKET_ERROR) break;

        if (sel > 0 && FD_ISSET(sock_, &fds)) {
            sockaddr_storage peer = {};
            int peer_len = sizeof(peer);
            int n = ::recvfrom(sock_,
                               reinterpret_cast<char*>(buf.data()),
                               static_cast<int>(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&peer),
                               &peer_len);
            if (n == SOCKET_ERROR) {
                if (!running_.load()) break;
                continue;
            }
            HandlePacket(buf.data(), static_cast<size_t>(n), peer,
                         static_cast<socklen_t>(peer_len));
        }

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

        ::sendto(sock_, reinterpret_cast<const char*>(out),
                 static_cast<int>(written), 0,
                 reinterpret_cast<const sockaddr*>(&si.to),
                 static_cast<int>(si.to_len));
    }

    // Schedule the next timer for this connection.
    uint64_t ns = quiche_conn_timeout_as_nanos(conn->qc);
    if (ns == UINT64_MAX) {
        conn->next_timeout = (std::chrono::steady_clock::time_point::max)();
    } else {
        conn->next_timeout = std::chrono::steady_clock::now() +
                             std::chrono::nanoseconds(ns);
    }
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
