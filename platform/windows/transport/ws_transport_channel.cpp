#include "ws_transport_channel.h"

#include "ws_transport_connection.h"

namespace lumen {

WsTransportChannel::WsTransportChannel(ChannelType type,
                                         WsTransportConnection* connection)
    : type_(type), connection_(connection) {
    switch (type) {
        case ChannelType::kControl: priority_ = SendPriority::kControl; break;
        case ChannelType::kAudio:   priority_ = SendPriority::kAudio;   break;
        case ChannelType::kVideo:   priority_ = SendPriority::kVideoDelta; break;
    }
}

Result<size_t> WsTransportChannel::Send(const uint8_t* data, size_t size,
                                          const SendOptions& /*options*/) {
    return connection_->SendBinaryFromChannel(type_, data, size);
}

void WsTransportChannel::SetReceiveCallback(DataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);
    receive_callback_ = std::move(callback);
}

size_t WsTransportChannel::GetSendCapacity() const {
    // Phase 1 sends synchronously under a mutex. Treat as effectively
    // unbounded so the encoder never sees backpressure-driven drops; a
    // proper async send queue lands as a follow-up.
    return GetMaxPayloadSize();
}

void WsTransportChannel::SetReadyToSendCallback(ReadyToSendCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);
    ready_to_send_callback_ = std::move(callback);
}

void WsTransportChannel::OnDataReceived(const uint8_t* data, size_t size) {
    DataReceivedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = receive_callback_;
    }
    if (cb) cb(data, size);
}

}  // namespace lumen
