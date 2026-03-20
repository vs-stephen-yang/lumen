#pragma once

#include "lumen/common/error.h"
#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <functional>
#include <string>

namespace lumen {

/// Callback for control messages received on the control channel.
using ControlMessageCallback =
    std::function<void(const uint8_t* data, size_t size)>;

/// Abstract interface for a transport connection.
///
/// A connection owns multiple logical channels (video, audio, control)
/// multiplexed over the same underlying transport.
class TransportConnection {
public:
    virtual ~TransportConnection() = default;

    /// Retrieve a channel by type.
    virtual TransportChannel* GetChannel(ChannelType type) = 0;

    /// Current connection state.
    virtual ConnectionState GetState() const = 0;

    /// Register for connection state changes.
    virtual void SetStateCallback(ConnectionStateCallback callback) = 0;

    /// Get current transport statistics.
    virtual TransportStats GetStats() const = 0;

    /// Register for periodic stats updates.
    virtual void SetStatsCallback(StatsCallback callback) = 0;

    /// Request the remote side to generate a keyframe.
    virtual Result<void> RequestKeyframe() = 0;

    /// Send a control message to the remote peer.
    virtual Result<void> SendControlMessage(const uint8_t* data,
                                            size_t size) = 0;

    /// Register for incoming control messages.
    virtual void SetControlMessageCallback(ControlMessageCallback callback) = 0;

    /// Gracefully close the connection.
    virtual void Close() = 0;

    /// Remote peer address as "host:port".
    virtual std::string GetRemoteAddress() const = 0;
};

}  // namespace lumen
