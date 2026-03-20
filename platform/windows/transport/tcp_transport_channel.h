#pragma once

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_types.h"

#include <functional>
#include <mutex>

namespace lumen {

class TcpTransportConnection;

/// TCP-based transport channel.
///
/// Logical channel over a shared TCP socket. Since TCP is a byte stream with
/// no MTU, frames are sent whole — no fragmentation/reassembly needed. The
/// wire framing [channel_id:u8][length:u32][payload] delineates messages.
/// Each payload is [MediaPacketHeader:28][frame data].
class TcpTransportChannel : public TransportChannel {
public:
    TcpTransportChannel(ChannelType type, TcpTransportConnection* connection);

    Result<size_t> Send(const uint8_t* data, size_t size,
                        const SendOptions& options) override;

    void SetReceiveCallback(DataReceivedCallback callback) override;
    size_t GetSendCapacity() const override;
    void SetReadyToSendCallback(ReadyToSendCallback callback) override;
    ChannelType GetType() const override { return type_; }
    ReliabilityMode GetReliability() const override {
        return ReliabilityMode::kReliableOrdered;
    }
    void SetPriority(SendPriority priority) override;
    size_t GetMaxPayloadSize() const override;

    /// Called by the connection when framed data arrives for this channel.
    void OnDataReceived(const uint8_t* data, size_t size);

    /// Fire the ready-to-send callback (called by connection after queue drain).
    void FireReadyToSend();

private:
    SendPriority DefaultPriority() const;

    ChannelType type_;
    TcpTransportConnection* connection_;  // Non-owning
    SendPriority priority_;
    mutable std::mutex mutex_;
    DataReceivedCallback receive_callback_;
    ReadyToSendCallback ready_to_send_callback_;
};

}  // namespace lumen
