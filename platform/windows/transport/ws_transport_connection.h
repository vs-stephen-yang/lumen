#pragma once

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"
#include "ws_transport_channel.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace lumen {

/// One peer's connection over a WebSocket.
///
/// Owns the SOCKET (handed off after the handshake by WsTransportServer),
/// runs a single receive thread, and serializes outbound writes under a
/// send mutex. Three logical channels (video, audio, control) share the
/// connection; they are dispatched by the first byte of each binary
/// message (the channel_id).
class WsTransportConnection : public TransportConnection {
public:
    /// `socket` is taken over by this connection (closed in dtor).
    /// `remote_address` is "host:port" for diagnostics.
    WsTransportConnection(SOCKET socket, std::string remote_address);
    ~WsTransportConnection() override;

    /// Start the receive thread. Returns once the thread is running.
    Result<void> Start();

    /// Send a binary WS message containing the payload preceded by the
    /// channel_id byte. Thread-safe.
    Result<size_t> SendBinaryFromChannel(ChannelType channel,
                                          const uint8_t* data, size_t size);

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

private:
    void ReceiveLoop();
    void DispatchBinary(const uint8_t* data, size_t size);
    void TransitionState(ConnectionState s);

    SOCKET socket_;
    std::string remote_address_;
    std::atomic<ConnectionState> state_{ConnectionState::kConnecting};

    std::mutex send_mu_;
    std::thread recv_thread_;
    std::atomic<bool> running_{false};

    std::unique_ptr<WsTransportChannel> video_;
    std::unique_ptr<WsTransportChannel> audio_;
    std::unique_ptr<WsTransportChannel> control_;

    std::mutex cb_mu_;
    ConnectionStateCallback state_callback_;
    ControlMessageCallback control_callback_;
};

}  // namespace lumen
