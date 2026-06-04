#include "quiche_client.h"

#include "wt_varint.h"

#include <quiche.h>

#include <bcrypt.h>

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

}  // namespace

QuicheClient::QuicheClient() = default;

QuicheClient::~QuicheClient() { Shutdown(); }

Result<void> QuicheClient::Initialize(const QuicheClientConfig& cfg) {
    cfg_ = cfg;

    quiche_cfg_ = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!quiche_cfg_) {
        return Error::Make(ErrorCode::kTransportError, "quiche_config_new failed");
    }

    if (quiche_config_set_application_protos(
            quiche_cfg_, cfg.alpn.data(), cfg.alpn.size()) != 0) {
        return Error::Make(ErrorCode::kTransportError,
                            "set_application_protos failed");
    }
    quiche_config_verify_peer(quiche_cfg_, cfg.verify_peer);
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
        quiche_config_enable_dgram(quiche_cfg_, true, cfg.dgram_recv_queue_len,
                                     cfg.dgram_send_queue_len);
    }

    h3_cfg_ = quiche_h3_config_new();
    if (!h3_cfg_) {
        return Error::Make(ErrorCode::kTransportError, "quiche_h3_config_new failed");
    }
    quiche_h3_config_enable_extended_connect(h3_cfg_, true);
    return {};
}

Result<void> QuicheClient::Start() {
    if (!quiche_cfg_ || !h3_cfg_) {
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

    sockaddr_in peer = {};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.host.c_str(), &peer.sin_addr) != 1) {
        sock_.reset();
        running_.store(false);
        return Error::Make(ErrorCode::kTransportAddressInvalid, cfg_.host);
    }
    std::memcpy(&peer_addr_, &peer, sizeof(peer));
    peer_addr_len_ = sizeof(peer);

    // connect() the UDP socket so the OS picks a concrete source address that
    // getsockname can report (needed as the local address in recv_info).
    if (auto r = sock_->Connect(reinterpret_cast<sockaddr*>(&peer),
                                sizeof(peer));
        !r) {
        sock_.reset();
        running_.store(false);
        return r.error();
    }

    if (auto r = sock_->GetLocalAddr(local_addr_, local_addr_len_); !r) {
        sock_.reset();
        running_.store(false);
        return r.error();
    }

    uint8_t scid[kLocalConnIdLen];
    if (!RandomBytes(scid, kLocalConnIdLen)) {
        sock_.reset();
        running_.store(false);
        return Error::Make(ErrorCode::kTransportError, "RandomBytes failed");
    }

    qc_ = quiche_connect(
        cfg_.authority.c_str(), scid, kLocalConnIdLen,
        reinterpret_cast<const sockaddr*>(&local_addr_), local_addr_len_,
        reinterpret_cast<const sockaddr*>(&peer_addr_), peer_addr_len_,
        quiche_cfg_);
    if (!qc_) {
        sock_.reset();
        running_.store(false);
        return Error::Make(ErrorCode::kTransportConnectionFailed,
                            "quiche_connect failed");
    }

    recv_thread_ = std::thread(&QuicheClient::RecvLoop, this);
    return {};
}

void QuicheClient::Stop() {
    if (!running_.exchange(false)) return;
    if (sock_) sock_->Close();  // unblocks WaitReadable in the recv loop
    if (recv_thread_.joinable()) recv_thread_.join();
}

void QuicheClient::Shutdown() {
    Stop();
    if (h3_) { quiche_h3_conn_free(h3_); h3_ = nullptr; }
    if (qc_) { quiche_conn_free(qc_); qc_ = nullptr; }
    if (h3_cfg_) { quiche_h3_config_free(h3_cfg_); h3_cfg_ = nullptr; }
    if (quiche_cfg_) { quiche_config_free(quiche_cfg_); quiche_cfg_ = nullptr; }
}

void QuicheClient::SetOnSession(SessionCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    session_cb_ = std::move(cb);
}

void QuicheClient::SetOnDatagram(DatagramCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    dgram_cb_ = std::move(cb);
}

Result<void> QuicheClient::SendWebTransportDatagram(uint64_t session_id,
                                                     const uint8_t* data,
                                                     size_t size) {
    uint8_t prefix[8];
    const size_t prefix_len = VarintEncode(session_id, prefix, sizeof(prefix));
    if (prefix_len == 0) {
        return Error::Make(ErrorCode::kInvalidArgument, "session_id too big");
    }
    std::vector<uint8_t> buf(prefix_len + size);
    std::memcpy(buf.data(), prefix, prefix_len);
    if (size) std::memcpy(buf.data() + prefix_len, data, size);
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        send_q_.push_back(std::move(buf));
    }
    return {};
}

Result<void> QuicheClient::GetConnectionStats(TransportStats& out) const {
    std::lock_guard<std::mutex> lock(stats_mu_);
    out = stats_;
    return {};
}

void QuicheClient::RecvLoop() {
    std::vector<uint8_t> buf(65535);

    // Kick the handshake (send the Initial).
    FlushEgress();

    while (running_.load()) {
        if (sock_->WaitReadable(50)) {
            sockaddr_storage from = {};
            socklen_t from_len = sizeof(from);
            int64_t n = sock_->Recv(buf.data(), buf.size(), from, from_len);
            if (n < 0) {
                if (!running_.load()) break;
            } else if (n > 0) {
                quiche_recv_info ri = {
                    reinterpret_cast<sockaddr*>(&from),
                    from_len,
                    reinterpret_cast<sockaddr*>(&local_addr_),
                    local_addr_len_,
                };
                (void)quiche_conn_recv(qc_, buf.data(),
                                       static_cast<size_t>(n), &ri);
            }
        } else {
            quiche_conn_on_timeout(qc_);
        }

        if (quiche_conn_is_closed(qc_)) break;

        if (quiche_conn_is_established(qc_)) {
            if (!h3_) h3_ = quiche_h3_conn_new_with_transport(qc_, h3_cfg_);
            MaybeSendConnect();
            DriveHttp3();
            DrainDatagrams();
        }

        DrainSendQueue();
        FlushEgress();
        UpdateStats();
    }
}

void QuicheClient::MaybeSendConnect() {
    if (sent_connect_ || !h3_) return;
    if (!quiche_h3_extended_connect_enabled_by_peer(h3_)) return;

    const auto* authority =
        reinterpret_cast<const uint8_t*>(cfg_.authority.c_str());
    const auto* path = reinterpret_cast<const uint8_t*>(cfg_.path.c_str());
    quiche_h3_header req[] = {
        {(const uint8_t*)":method",    7, (const uint8_t*)"CONNECT", 7},
        {(const uint8_t*)":protocol",  9, (const uint8_t*)"webtransport", 12},
        {(const uint8_t*)":scheme",    7, (const uint8_t*)"https", 5},
        {(const uint8_t*)":authority", 10, authority, cfg_.authority.size()},
        {(const uint8_t*)":path",      5, path, cfg_.path.size()},
    };
    connect_stream_ = quiche_h3_send_request(h3_, qc_, req, 5, /*fin=*/false);
    sent_connect_ = (connect_stream_ >= 0);
}

void QuicheClient::DriveHttp3() {
    if (!h3_) return;
    while (true) {
        quiche_h3_event* ev = nullptr;
        int64_t s = quiche_h3_conn_poll(h3_, qc_, &ev);
        if (s < 0) break;

        const auto type = quiche_h3_event_type(ev);
        if (type == QUICHE_H3_EVENT_HEADERS && s == connect_stream_ &&
            !session_established_.load()) {
            // 200 response on the CONNECT stream — the WT session is up.
            session_id_ = static_cast<uint64_t>(connect_stream_);
            session_established_.store(true);
            SessionCallback cb;
            {
                std::lock_guard<std::mutex> lock(cb_mu_);
                cb = session_cb_;
            }
            if (cb) cb(session_id_);
        }
        quiche_h3_event_free(ev);
    }
}

void QuicheClient::DrainDatagrams() {
    uint8_t buf[1500];
    while (true) {
        ssize_t n = quiche_conn_dgram_recv(qc_, buf, sizeof(buf));
        if (n <= 0) break;

        uint64_t sid = 0;
        const size_t consumed = VarintDecode(buf, static_cast<size_t>(n), &sid);
        if (consumed == 0) continue;

        DatagramCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mu_);
            cb = dgram_cb_;
        }
        if (cb) {
            cb(sid, buf + consumed, static_cast<size_t>(n) - consumed);
        }
    }
}

void QuicheClient::DrainSendQueue() {
    std::vector<std::vector<uint8_t>> batch;
    {
        std::lock_guard<std::mutex> lock(send_mu_);
        if (send_q_.empty()) return;
        batch.swap(send_q_);
    }
    for (auto& payload : batch) {
        (void)quiche_conn_dgram_send(qc_, payload.data(), payload.size());
    }
}

void QuicheClient::FlushEgress() {
    uint8_t out[kMaxDatagramSize];
    while (true) {
        quiche_send_info si = {};
        ssize_t written = quiche_conn_send(qc_, out, sizeof(out), &si);
        if (written == QUICHE_ERR_DONE) break;
        if (written < 0) return;
        sock_->SendTo(out, static_cast<size_t>(written),
                      reinterpret_cast<const sockaddr*>(&si.to), si.to_len);
    }
}

void QuicheClient::UpdateStats() {
    quiche_stats cs;
    std::memset(&cs, 0, sizeof(cs));
    quiche_conn_stats(qc_, &cs);

    quiche_path_stats ps;
    std::memset(&ps, 0, sizeof(ps));
    const bool have_path = quiche_conn_path_stats(qc_, 0, &ps) == 0;

    TransportStats t;
    t.bytes_sent = cs.sent_bytes;
    t.bytes_received = cs.recv_bytes;
    if (cs.sent > 0) {
        t.loss_rate =
            static_cast<double>(cs.lost) / static_cast<double>(cs.sent);
    }
    if (have_path) {
        t.rtt_us = ps.rtt / 1000;
        t.rtt_variance_us = ps.rttvar / 1000;
        t.bandwidth_estimate_bps = ps.delivery_rate * 8;
        t.congestion_window = ps.cwnd;
    }

    std::lock_guard<std::mutex> lock(stats_mu_);
    stats_ = t;
}

}  // namespace lumen
