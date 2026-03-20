#pragma once

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"
#include "quic_channel.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msquic.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace lumen {

/// QUIC-based transport connection.
///
/// Owns an HQUIC Connection, three logical channels (video on stream, audio on
/// datagram, control on stream), and routes msquic callbacks to the appropriate
/// channel. Video and control each get a dedicated bidirectional QUIC stream.
/// Audio uses QUIC datagrams (RFC 9221) for unreliable low-latency delivery.
///
/// Threading: all msquic callbacks fire on msquic's internal thread pool.
/// User-facing callbacks (receive, state) also fire on these threads.
class QuicTransportConnection
    : public TransportConnection,
      public std::enable_shared_from_this<QuicTransportConnection> {
public:
    /// Construct a QUIC connection wrapper.
    /// @param msquic   API table from MsQuicOpen2.
    /// @param is_server  True if this is the server (acceptor) side.
    /// @param remote_address  Remote peer address string.
    QuicTransportConnection(const QUIC_API_TABLE* msquic, bool is_server,
                            const std::string& remote_address);
    ~QuicTransportConnection() override;

    // TransportConnection interface
    TransportChannel* GetChannel(ChannelType type) override;
    ConnectionState GetState() const override;
    void SetStateCallback(ConnectionStateCallback callback) override;
    TransportStats GetStats() const override;
    void SetStatsCallback(StatsCallback callback) override;
    Result<void> RequestKeyframe() override;
    Result<void> SendControlMessage(const uint8_t* data, size_t size) override;
    void SetControlMessageCallback(ControlMessageCallback callback) override;
    void Close() override;
    std::string GetRemoteAddress() const override;

    /// Set the HQUIC Connection handle (called by server/client after creation).
    void SetHandle(HQUIC connection);

    /// Set a callback to fire when the connection reaches kConnected state.
    /// Used by client/server to deliver the connection to the user.
    void SetConnectCallback(
        std::function<void(std::shared_ptr<TransportConnection>)> callback);

    /// Send a QUIC datagram (connection-level, used by audio channel).
    /// Takes ownership of the QuicSendContext for lifetime management.
    QUIC_STATUS SendDatagram(QuicSendContext* ctx);

    /// msquic connection callback (static, dispatches to instance).
    static QUIC_STATUS QUIC_API ConnectionCallback(
        HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event);

    /// msquic stream callback (static, dispatches to instance).
    /// All streams use this callback with the connection as context.
    static QUIC_STATUS QUIC_API StreamRouterCallback(
        HQUIC stream, void* context, QUIC_STREAM_EVENT* event);

private:
    void OnConnected();
    void OnPeerStreamStarted(HQUIC stream, QUIC_STREAM_OPEN_FLAGS flags);
    void OnDatagramReceived(const QUIC_BUFFER* buffer);
    void OnDatagramStateChanged(bool send_enabled, uint16_t max_send_length);
    void OnDatagramSendStateChanged(void* context, QUIC_DATAGRAM_SEND_STATE state);
    void OnShutdownInitiated();
    void OnShutdownComplete(bool app_close_in_progress);

    void OpenClientStreams();
    void HandleStreamReceive(HQUIC stream, const QUIC_BUFFER* buffers,
                             uint32_t buffer_count, uint64_t total_length);
    void HandleUnassignedStreamData(HQUIC stream, const QUIC_BUFFER* buffers,
                                     uint32_t buffer_count);
    void SetState(ConnectionState state);

    QuicChannel* ChannelByType(ChannelType type);
    QuicChannel* FindChannelForStream(HQUIC stream);

    const QUIC_API_TABLE* msquic_;
    HQUIC connection_ = nullptr;
    bool is_server_;
    std::string remote_address_;
    std::atomic<ConnectionState> state_{ConnectionState::kConnecting};

    std::unique_ptr<QuicChannel> video_channel_;
    std::unique_ptr<QuicChannel> audio_channel_;
    std::unique_ptr<QuicChannel> control_channel_;

    std::atomic<uint64_t> control_frame_index_{0};

    // Stream → channel mapping
    std::mutex stream_map_mutex_;
    std::unordered_map<HQUIC, QuicChannel*> stream_to_channel_;

    // Callbacks
    mutable std::mutex callback_mutex_;
    ConnectionStateCallback state_callback_;
    StatsCallback stats_callback_;
    ControlMessageCallback control_callback_;
    std::function<void(std::shared_ptr<TransportConnection>)> connect_callback_;

    // Stats
    mutable std::mutex stats_mutex_;
    TransportStats stats_;
};

}  // namespace lumen
