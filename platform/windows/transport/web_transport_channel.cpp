#include "web_transport_channel.h"

#include <cstring>
#include <vector>

namespace lumen {

WebTransportChannel::WebTransportChannel(ChannelType type,
                                          WebTransportDatagramSink* sink)
    : type_(type),
      sink_(sink),
      fragmenter_(kFragmentPayload) {
    switch (type) {
        case ChannelType::kControl: priority_ = SendPriority::kControl;    break;
        case ChannelType::kAudio:   priority_ = SendPriority::kAudio;      break;
        case ChannelType::kVideo:   priority_ = SendPriority::kVideoDelta; break;
    }

    // On reassembly, re-prepend the 28-byte header so the consumer receives
    // [MediaPacketHeader:28][frame] (matches the WS/TCP channel convention).
    reassembler_.SetCallback(
        [this](const uint8_t* payload, size_t size, const MediaPacketHeader& h) {
            DataReceivedCallback cb;
            {
                std::lock_guard<std::mutex> lock(mu_);
                cb = receive_callback_;
            }
            if (!cb) return;

            std::vector<uint8_t> out(MediaPacketHeader::kSerializedSize + size);
            h.Serialize(out.data());
            if (size) {
                std::memcpy(out.data() + MediaPacketHeader::kSerializedSize,
                            payload, size);
            }
            cb(out.data(), out.size());
        });
}

Result<size_t> WebTransportChannel::Send(const uint8_t* data, size_t size,
                                          const SendOptions& options) {
    MediaPacketHeader tmpl;
    tmpl.ssrc = 0;
    tmpl.frame_index = options.frame_index;
    tmpl.timestamp_us = options.timestamp_us;
    tmpl.SetKeyframe(options.is_keyframe);

    auto fragments = fragmenter_.Fragment(data, size, tmpl);
    for (auto& frag : fragments) {
        auto r = sink_->SendChannelDatagram(type_, frag.data.data(),
                                            frag.data.size());
        if (!r) return r.error();
    }
    return size;
}

void WebTransportChannel::SetReceiveCallback(DataReceivedCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);
    receive_callback_ = std::move(callback);
}

size_t WebTransportChannel::GetSendCapacity() const {
    // Datagrams are fire-and-forget; report a non-zero capacity so the
    // encoder never sees backpressure-driven drops. A proper estimate based
    // on quiche's dgram send queue is a follow-up.
    return GetMaxPayloadSize();
}

void WebTransportChannel::SetReadyToSendCallback(ReadyToSendCallback callback) {
    std::lock_guard<std::mutex> lock(mu_);
    ready_to_send_callback_ = std::move(callback);
}

void WebTransportChannel::OnDatagramFragment(const uint8_t* frag, size_t size) {
    reassembler_.AddFragment(frag, size);
}

}  // namespace lumen
