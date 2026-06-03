#include "web_transport_client_connection.h"

#include "quiche_client.h"
#include "lumen/transport/wire_format.h"

#include <cstring>
#include <vector>

namespace lumen {

WebTransportClientConnection::WebTransportClientConnection(
    QuicheClient* client, uint64_t session_id, std::string remote_address)
    : client_(client),
      session_id_(session_id),
      remote_address_(std::move(remote_address)) {
    video_   = std::make_unique<WebTransportChannel>(ChannelType::kVideo,   this);
    audio_   = std::make_unique<WebTransportChannel>(ChannelType::kAudio,   this);
    control_ = std::make_unique<WebTransportChannel>(ChannelType::kControl, this);
}

WebTransportClientConnection::~WebTransportClientConnection() = default;

TransportChannel* WebTransportClientConnection::GetChannel(ChannelType type) {
    return ChannelByType(type);
}

WebTransportChannel* WebTransportClientConnection::ChannelByType(
    ChannelType type) {
    switch (type) {
        case ChannelType::kVideo:   return video_.get();
        case ChannelType::kAudio:   return audio_.get();
        case ChannelType::kControl: return control_.get();
    }
    return nullptr;
}

TransportStats WebTransportClientConnection::GetStats() const {
    TransportStats out;
    if (client_) client_->GetConnectionStats(out);
    return out;
}

void WebTransportClientConnection::SetStateCallback(
    ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    state_callback_ = std::move(callback);
}

Result<void> WebTransportClientConnection::RequestKeyframe() {
    return {};
}

Result<void> WebTransportClientConnection::SendControlMessage(
    const uint8_t* data, size_t size) {
    auto* ch = control_.get();
    if (!ch) return Error::Make(ErrorCode::kTransportError, "no control channel");
    auto r = ch->Send(data, size, SendOptions{});
    if (!r) return r.error();
    return {};
}

void WebTransportClientConnection::SetControlMessageCallback(
    ControlMessageCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    control_callback_ = std::move(callback);
}

void WebTransportClientConnection::Close() {
    TransitionState(ConnectionState::kDisconnected);
}

Result<size_t> WebTransportClientConnection::SendChannelDatagram(
    ChannelType type, const uint8_t* frag, size_t size) {
    std::vector<uint8_t> buf(size + 1);
    buf[0] = wire::ChannelIdOf(type);
    if (size) std::memcpy(buf.data() + 1, frag, size);

    auto r = client_->SendWebTransportDatagram(session_id_, buf.data(),
                                               buf.size());
    if (!r) return r.error();
    return size;
}

void WebTransportClientConnection::OnDatagram(const uint8_t* data,
                                               size_t size) {
    if (size < 1) return;
    ChannelType channel_id;
    if (!wire::ChannelTypeOf(data[0], channel_id)) return;
    auto* ch = ChannelByType(channel_id);
    if (!ch) return;
    ch->OnDatagramFragment(data + 1, size - 1);
}

void WebTransportClientConnection::TransitionState(ConnectionState s) {
    state_.store(s);
    ConnectionStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb = state_callback_;
    }
    if (cb) cb(s);
}

}  // namespace lumen
