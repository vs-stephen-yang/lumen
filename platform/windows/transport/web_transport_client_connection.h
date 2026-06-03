#pragma once

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"
#include "web_transport_channel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace lumen {

class QuicheClient;

/// Client-side WebTransport connection: a single established session over a
/// QuicheClient, exposing video/audio/control channels behind the abstract
/// TransportConnection. The mirror image of WebTransportConnection (server
/// side); both share WebTransportChannel via the WebTransportDatagramSink.
class WebTransportClientConnection : public TransportConnection,
                                      public WebTransportDatagramSink {
public:
    WebTransportClientConnection(QuicheClient* client, uint64_t session_id,
                                 std::string remote_address);
    ~WebTransportClientConnection() override;

    // TransportConnection interface
    TransportChannel* GetChannel(ChannelType type) override;
    ConnectionState GetState() const override { return state_.load(); }
    void SetStateCallback(ConnectionStateCallback callback) override;
    TransportStats GetStats() const override;
    void SetStatsCallback(StatsCallback /*callback*/) override {}
    Result<void> RequestKeyframe() override;
    Result<void> SendControlMessage(const uint8_t* data, size_t size) override;
    void SetControlMessageCallback(ControlMessageCallback callback) override;
    void Close() override;
    std::string GetRemoteAddress() const override { return remote_address_; }

    // WebTransportDatagramSink
    Result<size_t> SendChannelDatagram(ChannelType type, const uint8_t* frag,
                                       size_t size) override;

    /// Route an inbound WT datagram ([channel_id][header][fragment]) to the
    /// right channel. Called on the QuicheClient recv thread.
    void OnDatagram(const uint8_t* data, size_t size);

    void TransitionState(ConnectionState s);

private:
    WebTransportChannel* ChannelByType(ChannelType type);

    QuicheClient* client_;  // Non-owning.
    uint64_t session_id_;
    std::string remote_address_;
    std::atomic<ConnectionState> state_{ConnectionState::kConnected};

    std::unique_ptr<WebTransportChannel> video_;
    std::unique_ptr<WebTransportChannel> audio_;
    std::unique_ptr<WebTransportChannel> control_;

    std::mutex cb_mu_;
    ConnectionStateCallback state_callback_;
    ControlMessageCallback control_callback_;
};

}  // namespace lumen
