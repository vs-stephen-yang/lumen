#pragma once

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <mutex>

namespace lumen {

class WsTransportConnection;

/// WebSocket-backed transport channel.
///
/// Wire convention (matches docs/web-sender-implementation-plan.md §S5):
///   WS binary message body = [channel_id:u8][MediaPacketHeader:28][frame]
///
/// channel_id is the underlying ChannelType cast to a byte. Receivers
/// dispatch by inspecting that first byte; the receive callback fires
/// with the [header + payload] portion.
class WsTransportChannel : public TransportChannel {
public:
    WsTransportChannel(ChannelType type, WsTransportConnection* connection);

    Result<size_t> Send(const uint8_t* data, size_t size,
                        const SendOptions& options) override;

    void SetReceiveCallback(DataReceivedCallback callback) override;
    size_t GetSendCapacity() const override;
    void SetReadyToSendCallback(ReadyToSendCallback callback) override;
    ChannelType GetType() const override { return type_; }
    ReliabilityMode GetReliability() const override {
        return ReliabilityMode::kReliableOrdered;
    }
    void SetPriority(SendPriority priority) override { priority_ = priority; }
    size_t GetMaxPayloadSize() const override { return 16 * 1024 * 1024; }

    /// Called by the connection when a binary WS message arrives that
    /// belongs to this channel (channel_id byte already stripped).
    void OnDataReceived(const uint8_t* data, size_t size);

private:
    ChannelType type_;
    WsTransportConnection* connection_;  // Non-owning.
    SendPriority priority_ = SendPriority::kBulk;
    mutable std::mutex mu_;
    DataReceivedCallback receive_callback_;
    ReadyToSendCallback ready_to_send_callback_;
};

}  // namespace lumen
