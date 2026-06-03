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

class QuicheServer;

/// WebTransport-backed transport connection.
///
/// Wraps a single WebTransport session (identified by a QUIC connection id +
/// HTTP/3 session stream id) hosted by a QuicheServer. Owns three logical
/// channels (video/audio/control) multiplexed over WT datagrams. Inbound
/// datagrams carry a leading channel_id byte that selects the channel.
///
/// Threading: OnDatagram() fires on the QuicheServer recv thread; receive
/// callbacks therefore fire on that thread (per the TransportChannel
/// contract — consumers must not block).
class WebTransportConnection : public TransportConnection {
public:
    WebTransportConnection(QuicheServer* server, std::string conn_id,
                           uint64_t session_id, std::string remote_address);
    ~WebTransportConnection() override;

    // TransportConnection interface
    TransportChannel* GetChannel(ChannelType type) override;
    ConnectionState GetState() const override { return state_.load(); }
    void SetStateCallback(ConnectionStateCallback callback) override;
    TransportStats GetStats() const override { return {}; }
    void SetStatsCallback(StatsCallback /*callback*/) override {}
    Result<void> RequestKeyframe() override;
    Result<void> SendControlMessage(const uint8_t* data, size_t size) override;
    void SetControlMessageCallback(ControlMessageCallback callback) override;
    void Close() override;
    std::string GetRemoteAddress() const override { return remote_address_; }

    /// Route an inbound WT datagram for this session to the right channel.
    /// `data` is [channel_id:u8][MediaPacketHeader:28][fragment] (the
    /// session_id varint has already been stripped by QuicheServer).
    void OnDatagram(const uint8_t* data, size_t size);

    /// Send one fragment (called by a channel). `frag` is
    /// [MediaPacketHeader:28][fragment]; the channel_id byte is prepended
    /// here before handing off to the QuicheServer.
    Result<size_t> SendDatagram(ChannelType type, const uint8_t* frag,
                                size_t size);

    void TransitionState(ConnectionState s);

private:
    WebTransportChannel* ChannelByType(ChannelType type);

    QuicheServer* server_;  // Non-owning.
    std::string conn_id_;
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
