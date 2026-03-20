#pragma once

#include "lumen/common/error.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <functional>

namespace lumen {

/// Options for a single send operation.
struct SendOptions {
    uint64_t timestamp_us = 0;
    uint64_t frame_index = 0;
    bool is_keyframe = false;
};

/// Callback fired when the channel is ready to accept more data.
using ReadyToSendCallback = std::function<void()>;

/// Abstract interface for a logical transport channel (video, audio, or control).
///
/// Each channel type may have different reliability and priority defaults.
/// Implementations fragment outgoing frames and reassemble incoming ones.
class TransportChannel {
public:
    virtual ~TransportChannel() = default;

    /// Send a frame on this channel. The implementation handles fragmentation.
    /// @return Number of bytes queued, or an error.
    virtual Result<size_t> Send(const uint8_t* data, size_t size,
                                const SendOptions& options) = 0;

    /// Set the callback for received (reassembled) frames.
    virtual void SetReceiveCallback(DataReceivedCallback callback) = 0;

    /// Bytes available in the send buffer before backpressure.
    virtual size_t GetSendCapacity() const = 0;

    /// Register for notification when send capacity becomes available.
    virtual void SetReadyToSendCallback(ReadyToSendCallback callback) = 0;

    /// The logical channel type.
    virtual ChannelType GetType() const = 0;

    /// The reliability mode of this channel.
    virtual ReliabilityMode GetReliability() const = 0;

    /// Set the send priority for this channel.
    virtual void SetPriority(SendPriority priority) = 0;

    /// Maximum payload size per packet (excluding transport header overhead).
    virtual size_t GetMaxPayloadSize() const = 0;
};

}  // namespace lumen
