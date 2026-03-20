#include "quic_channel.h"
#include "quic_transport_connection.h"

#include <cstring>

namespace lumen {

QuicChannel::QuicChannel(ChannelType type, QuicTransportConnection* connection,
                         Mode mode, const QUIC_API_TABLE* msquic)
    : type_(type),
      connection_(connection),
      mode_(mode),
      msquic_(msquic),
      priority_(DefaultPriority()),
      fragmenter_(1200 - MediaPacketHeader::kSerializedSize),
      reassembler_(500'000) {
    if (mode_ == Mode::kDatagram) {
        reassembler_.SetCallback(
            [this](const uint8_t* data, size_t size, const MediaPacketHeader&) {
                DataReceivedCallback cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cb = receive_callback_;
                }
                if (cb) cb(data, size);
            });
    }
}

QuicChannel::~QuicChannel() = default;

SendPriority QuicChannel::DefaultPriority() const {
    switch (type_) {
        case ChannelType::kControl: return SendPriority::kControl;
        case ChannelType::kAudio:   return SendPriority::kAudio;
        case ChannelType::kVideo:   return SendPriority::kVideoDelta;
    }
    return SendPriority::kBulk;
}

ReliabilityMode QuicChannel::GetReliability() const {
    return (mode_ == Mode::kStream) ? ReliabilityMode::kReliableOrdered
                                    : ReliabilityMode::kUnreliable;
}

Result<size_t> QuicChannel::Send(const uint8_t* data, size_t size,
                                  const SendOptions& options) {
    if (mode_ == Mode::kStream) {
        return SendOnStream(data, size, options);
    }
    return SendOnDatagram(data, size, options);
}

Result<size_t> QuicChannel::SendOnStream(const uint8_t* data, size_t size,
                                          const SendOptions& options) {
    if (!stream_) {
        return Error::Make(ErrorCode::kTransportNotConnected,
                          "QUIC stream not yet established");
    }

    // Build header.
    MediaPacketHeader hdr;
    hdr.ssrc = static_cast<uint32_t>(type_);
    hdr.frame_index = options.frame_index;
    hdr.timestamp_us = options.timestamp_us;
    hdr.fragment_index = 0;
    hdr.fragment_count = 1;
    hdr.SetKeyframe(options.is_keyframe);
    hdr.SetLastFragment(true);

    // Wire format: [length:u32][MediaPacketHeader:28][payload]
    uint32_t msg_len = static_cast<uint32_t>(
        MediaPacketHeader::kSerializedSize + size);

    auto* ctx = new QuicSendContext();
    ctx->data.resize(kStreamFrameHeaderSize + msg_len);

    // Length prefix (big-endian).
    ctx->data[0] = static_cast<uint8_t>((msg_len >> 24) & 0xFF);
    ctx->data[1] = static_cast<uint8_t>((msg_len >> 16) & 0xFF);
    ctx->data[2] = static_cast<uint8_t>((msg_len >> 8) & 0xFF);
    ctx->data[3] = static_cast<uint8_t>(msg_len & 0xFF);

    // Serialize header.
    hdr.Serialize(ctx->data.data() + kStreamFrameHeaderSize);

    // Copy payload.
    if (size > 0) {
        std::memcpy(ctx->data.data() + kStreamFrameHeaderSize +
                        MediaPacketHeader::kSerializedSize,
                    data, size);
    }

    ctx->Prepare();

    QUIC_STATUS status = msquic_->StreamSend(
        stream_, &ctx->buffer, 1, QUIC_SEND_FLAG_NONE, ctx);
    if (QUIC_FAILED(status)) {
        delete ctx;
        return Error::Make(ErrorCode::kTransportError, "StreamSend failed");
    }

    return ctx->data.size();
}

Result<size_t> QuicChannel::SendOnDatagram(const uint8_t* data, size_t size,
                                            const SendOptions& options) {
    MediaPacketHeader hdr;
    hdr.ssrc = static_cast<uint32_t>(type_);
    hdr.frame_index = options.frame_index;
    hdr.timestamp_us = options.timestamp_us;
    hdr.SetKeyframe(options.is_keyframe);

    // Fragment into datagram-sized packets.
    auto fragments = fragmenter_.Fragment(data, size, hdr);
    size_t total = 0;

    for (auto& frag : fragments) {
        auto* ctx = new QuicSendContext();
        size_t frag_size = frag.data.size();
        ctx->data = std::move(frag.data);
        ctx->Prepare();

        QUIC_STATUS status = connection_->SendDatagram(ctx);
        if (QUIC_FAILED(status)) {
            delete ctx;
            return Error::Make(ErrorCode::kTransportError,
                              "DatagramSend failed");
        }
        total += frag_size;
    }

    return total;
}

void QuicChannel::SetReceiveCallback(DataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    receive_callback_ = std::move(callback);
}

size_t QuicChannel::GetSendCapacity() const {
    // QUIC handles flow control internally. Report a large capacity.
    return 4 * 1024 * 1024;
}

void QuicChannel::SetReadyToSendCallback(ReadyToSendCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    ready_to_send_callback_ = std::move(callback);
}

void QuicChannel::SetPriority(SendPriority priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    priority_ = priority;
}

size_t QuicChannel::GetMaxPayloadSize() const {
    if (mode_ == Mode::kDatagram) {
        return max_datagram_size_ > MediaPacketHeader::kSerializedSize
                   ? max_datagram_size_ - MediaPacketHeader::kSerializedSize
                   : 0;
    }
    return kMaxRecvMessageSize;
}

void QuicChannel::SetStream(HQUIC stream) {
    stream_ = stream;
}

void QuicChannel::SetMaxDatagramSize(uint16_t size) {
    max_datagram_size_ = size;
    // Update fragmenter with new max payload (minus header).
    size_t max_payload = (size > MediaPacketHeader::kSerializedSize)
                             ? size - MediaPacketHeader::kSerializedSize
                             : 0;
    fragmenter_ = FrameFragmenter(max_payload > 0 ? max_payload : 1);
}

void QuicChannel::OnStreamDataReceived(const QUIC_BUFFER* buffers,
                                        uint32_t buffer_count) {
    // Append all incoming data to the recv buffer.
    for (uint32_t i = 0; i < buffer_count; i++) {
        stream_recv_buf_.insert(stream_recv_buf_.end(),
                                buffers[i].Buffer,
                                buffers[i].Buffer + buffers[i].Length);
    }
    ProcessStreamRecvBuffer();
}

void QuicChannel::ProcessStreamRecvBuffer() {
    // Parse length-prefixed messages: [length:u32][payload].
    while (stream_recv_buf_.size() >= kStreamFrameHeaderSize) {
        uint32_t msg_len =
            (static_cast<uint32_t>(stream_recv_buf_[0]) << 24) |
            (static_cast<uint32_t>(stream_recv_buf_[1]) << 16) |
            (static_cast<uint32_t>(stream_recv_buf_[2]) << 8) |
            static_cast<uint32_t>(stream_recv_buf_[3]);

        if (msg_len > kMaxRecvMessageSize) {
            stream_recv_buf_.clear();
            return;
        }

        size_t total_needed = kStreamFrameHeaderSize + msg_len;
        if (stream_recv_buf_.size() < total_needed) break;

        // Complete message: [MediaPacketHeader:28][frame data]
        const uint8_t* msg_data =
            stream_recv_buf_.data() + kStreamFrameHeaderSize;

        if (msg_len >= MediaPacketHeader::kSerializedSize) {
            MediaPacketHeader hdr;
            if (MediaPacketHeader::Deserialize(msg_data, msg_len, hdr)) {
                const uint8_t* frame_data =
                    msg_data + MediaPacketHeader::kSerializedSize;
                size_t frame_size =
                    msg_len - MediaPacketHeader::kSerializedSize;

                DataReceivedCallback cb;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    cb = receive_callback_;
                }
                if (cb) cb(frame_data, frame_size);
            }
        }

        // Remove consumed data.
        stream_recv_buf_.erase(stream_recv_buf_.begin(),
                                stream_recv_buf_.begin() + total_needed);
    }
}

void QuicChannel::OnDatagramReceived(const uint8_t* data, size_t size) {
    // Each datagram is a complete fragment: [MediaPacketHeader:28][payload]
    reassembler_.AddFragment(data, size);
}

void QuicChannel::FireReadyToSend() {
    ReadyToSendCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = ready_to_send_callback_;
    }
    if (cb) cb();
}

}  // namespace lumen
