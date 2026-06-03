#pragma once

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_types.h"
#include "frame_fragmenter.h"
#include "frame_reassembler.h"

#include <mutex>

namespace lumen {

class WebTransportConnection;

/// WebTransport-backed transport channel (datagram mode).
///
/// Each logical channel (video/audio/control) carries MTU-sized fragments
/// inside WebTransport datagrams. Wire convention (channel_id is prepended
/// by the owning WebTransportConnection, not here):
///   WT datagram payload = [channel_id:u8][MediaPacketHeader:28][fragment]
///
/// On receive, fragments are reassembled by an internal FrameReassembler.
/// To match the WS/TCP convention (the receive callback delivers the full
/// [header + payload] so the consumer can read frame_index / keyframe), the
/// 28-byte MediaPacketHeader is re-serialized in front of the reassembled
/// frame before the DataReceivedCallback fires.
class WebTransportChannel : public TransportChannel {
public:
    WebTransportChannel(ChannelType type, WebTransportConnection* connection);

    // TransportChannel interface
    Result<size_t> Send(const uint8_t* data, size_t size,
                        const SendOptions& options) override;
    void SetReceiveCallback(DataReceivedCallback callback) override;
    size_t GetSendCapacity() const override;
    void SetReadyToSendCallback(ReadyToSendCallback callback) override;
    ChannelType GetType() const override { return type_; }
    ReliabilityMode GetReliability() const override {
        return ReliabilityMode::kUnreliable;
    }
    void SetPriority(SendPriority priority) override { priority_ = priority; }
    size_t GetMaxPayloadSize() const override { return kFragmentPayload; }

    /// Called by the connection for each inbound datagram routed to this
    /// channel. `frag` is [MediaPacketHeader:28][fragment payload] — the
    /// channel_id byte has already been stripped by the connection.
    void OnDatagramFragment(const uint8_t* frag, size_t size);

private:
    // Conservative payload budget per datagram fragment (excludes the
    // 28-byte MediaPacketHeader). Leaves room for the session_id varint and
    // channel_id byte within quiche's ~1350-byte datagram limit.
    static constexpr size_t kFragmentPayload = 1100;

    ChannelType type_;
    WebTransportConnection* connection_;  // Non-owning.
    SendPriority priority_;

    FrameFragmenter fragmenter_;
    FrameReassembler reassembler_;

    mutable std::mutex mu_;
    DataReceivedCallback receive_callback_;
    ReadyToSendCallback ready_to_send_callback_;
};

}  // namespace lumen
