#include "tcp_transport_channel.h"
#include "tcp_transport_connection.h"

#include <cstring>

namespace lumen {

TcpTransportChannel::TcpTransportChannel(ChannelType type,
                                         TcpTransportConnection* connection)
    : type_(type),
      connection_(connection),
      priority_(DefaultPriority()) {}

SendPriority TcpTransportChannel::DefaultPriority() const {
    switch (type_) {
        case ChannelType::kControl: return SendPriority::kControl;
        case ChannelType::kAudio:   return SendPriority::kAudio;
        case ChannelType::kVideo:   return SendPriority::kVideoDelta;
    }
    return SendPriority::kBulk;
}

Result<size_t> TcpTransportChannel::Send(const uint8_t* data, size_t size,
                                          const SendOptions& options) {
    // Build a MediaPacketHeader for this frame.
    MediaPacketHeader hdr;
    hdr.ssrc = static_cast<uint32_t>(type_);
    hdr.frame_index = options.frame_index;
    hdr.timestamp_us = options.timestamp_us;
    hdr.fragment_index = 0;
    hdr.fragment_count = 1;
    hdr.SetKeyframe(options.is_keyframe);
    hdr.SetLastFragment(true);

    SendPriority prio;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prio = priority_;
    }

    // For video keyframes, bump priority.
    if (type_ == ChannelType::kVideo && options.is_keyframe) {
        prio = SendPriority::kVideoKey;
    }

    // TCP is a byte stream — no MTU, no fragmentation needed.
    // Serialize header + frame payload into a single buffer.
    std::vector<uint8_t> payload(MediaPacketHeader::kSerializedSize + size);
    hdr.Serialize(payload.data());
    if (size > 0) {
        std::memcpy(payload.data() + MediaPacketHeader::kSerializedSize, data, size);
    }

    size_t total_size = payload.size();
    if (!connection_->EnqueueSend(static_cast<uint8_t>(type_),
                                   std::move(payload), prio)) {
        return Error::Make(ErrorCode::kTransportChannelFull);
    }

    return total_size;
}

void TcpTransportChannel::SetReceiveCallback(DataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    receive_callback_ = std::move(callback);
}

size_t TcpTransportChannel::GetSendCapacity() const {
    return connection_->GetSendQueueCapacity();
}

void TcpTransportChannel::SetReadyToSendCallback(ReadyToSendCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_to_send_callback_ = std::move(callback);
}

void TcpTransportChannel::SetPriority(SendPriority priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    priority_ = priority;
}

size_t TcpTransportChannel::GetMaxPayloadSize() const {
    // TCP has no MTU limit; return the max recv payload as a practical limit.
    return 2 * 1024 * 1024;
}

void TcpTransportChannel::OnDataReceived(const uint8_t* data, size_t size) {
    // Payload is [MediaPacketHeader:28][frame data].
    if (size < MediaPacketHeader::kSerializedSize) return;

    MediaPacketHeader hdr;
    if (!MediaPacketHeader::Deserialize(data, size, hdr)) return;

    const uint8_t* frame_data = data + MediaPacketHeader::kSerializedSize;
    size_t frame_size = size - MediaPacketHeader::kSerializedSize;

    DataReceivedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = receive_callback_;
    }
    if (cb) cb(frame_data, frame_size);
}

void TcpTransportChannel::FireReadyToSend() {
    ReadyToSendCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = ready_to_send_callback_;
    }
    if (cb) cb();
}

}  // namespace lumen
