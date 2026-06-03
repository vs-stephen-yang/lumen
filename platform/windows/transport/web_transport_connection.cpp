#include "web_transport_connection.h"

#include "quiche_server.h"

#include <cstring>
#include <vector>

namespace lumen {

WebTransportConnection::WebTransportConnection(QuicheServer* server,
                                                std::string conn_id,
                                                uint64_t session_id,
                                                std::string remote_address)
    : server_(server),
      conn_id_(std::move(conn_id)),
      session_id_(session_id),
      remote_address_(std::move(remote_address)) {
    video_   = std::make_unique<WebTransportChannel>(ChannelType::kVideo,   this);
    audio_   = std::make_unique<WebTransportChannel>(ChannelType::kAudio,   this);
    control_ = std::make_unique<WebTransportChannel>(ChannelType::kControl, this);
}

WebTransportConnection::~WebTransportConnection() = default;

TransportChannel* WebTransportConnection::GetChannel(ChannelType type) {
    return ChannelByType(type);
}

WebTransportChannel* WebTransportConnection::ChannelByType(ChannelType type) {
    switch (type) {
        case ChannelType::kVideo:   return video_.get();
        case ChannelType::kAudio:   return audio_.get();
        case ChannelType::kControl: return control_.get();
    }
    return nullptr;
}

TransportStats WebTransportConnection::GetStats() const {
    TransportStats out;
    if (server_) server_->GetConnectionStats(conn_id_, out);
    return out;
}

void WebTransportConnection::SetStateCallback(ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    state_callback_ = std::move(callback);
}

Result<void> WebTransportConnection::RequestKeyframe() {
    // The control channel carries arbitrary application bytes; the keyframe
    // request format is owned by the application. Nothing to do at this layer.
    return {};
}

Result<void> WebTransportConnection::SendControlMessage(const uint8_t* data,
                                                         size_t size) {
    auto* ch = control_.get();
    if (!ch) return Error::Make(ErrorCode::kTransportError, "no control channel");
    auto r = ch->Send(data, size, SendOptions{});
    if (!r) return r.error();
    return {};
}

void WebTransportConnection::SetControlMessageCallback(
    ControlMessageCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    control_callback_ = std::move(callback);
}

void WebTransportConnection::Close() {
    TransitionState(ConnectionState::kDisconnected);
}

void WebTransportConnection::OnDatagram(const uint8_t* data, size_t size) {
    if (size < 1) return;
    const auto channel_id = static_cast<ChannelType>(data[0]);
    auto* ch = ChannelByType(channel_id);
    if (!ch) return;
    ch->OnDatagramFragment(data + 1, size - 1);
}

Result<size_t> WebTransportConnection::SendDatagram(ChannelType type,
                                                     const uint8_t* frag,
                                                     size_t size) {
    // Prepend the channel_id byte: [channel_id:u8][MediaPacketHeader:28][frag].
    std::vector<uint8_t> buf(size + 1);
    buf[0] = static_cast<uint8_t>(type);
    if (size) std::memcpy(buf.data() + 1, frag, size);

    auto r = server_->SendWebTransportDatagram(conn_id_, session_id_,
                                               buf.data(), buf.size());
    if (!r) return r.error();
    return size;
}

void WebTransportConnection::TransitionState(ConnectionState s) {
    state_.store(s);
    ConnectionStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb = state_callback_;
    }
    if (cb) cb(s);
}

}  // namespace lumen
