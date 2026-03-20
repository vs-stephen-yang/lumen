#include "quic_transport_connection.h"

#include <cstring>

namespace lumen {

QuicTransportConnection::QuicTransportConnection(
    const QUIC_API_TABLE* msquic, bool is_server,
    const std::string& remote_address)
    : msquic_(msquic), is_server_(is_server), remote_address_(remote_address) {
    video_channel_ = std::make_unique<QuicChannel>(
        ChannelType::kVideo, this, QuicChannel::Mode::kStream, msquic_);
    audio_channel_ = std::make_unique<QuicChannel>(
        ChannelType::kAudio, this, QuicChannel::Mode::kDatagram, msquic_);
    control_channel_ = std::make_unique<QuicChannel>(
        ChannelType::kControl, this, QuicChannel::Mode::kStream, msquic_);
}

QuicTransportConnection::~QuicTransportConnection() {
    Close();
}

void QuicTransportConnection::SetHandle(HQUIC connection) {
    connection_ = connection;
}

TransportChannel* QuicTransportConnection::GetChannel(ChannelType type) {
    return ChannelByType(type);
}

QuicChannel* QuicTransportConnection::ChannelByType(ChannelType type) {
    switch (type) {
        case ChannelType::kVideo:   return video_channel_.get();
        case ChannelType::kAudio:   return audio_channel_.get();
        case ChannelType::kControl: return control_channel_.get();
    }
    return nullptr;
}

ConnectionState QuicTransportConnection::GetState() const {
    return state_.load();
}

void QuicTransportConnection::SetStateCallback(
    ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

TransportStats QuicTransportConnection::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void QuicTransportConnection::SetStatsCallback(StatsCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    stats_callback_ = std::move(callback);
}

Result<void> QuicTransportConnection::RequestKeyframe() {
    static const uint8_t kKeyframeRequest[] = {'K', 'F', 'R', 'Q'};
    return SendControlMessage(kKeyframeRequest, sizeof(kKeyframeRequest));
}

Result<void> QuicTransportConnection::SendControlMessage(const uint8_t* data,
                                                          size_t size) {
    if (state_.load() != ConnectionState::kConnected) {
        return Error::Make(ErrorCode::kTransportNotConnected);
    }

    SendOptions opts;
    opts.frame_index = control_frame_index_++;
    opts.timestamp_us = 0;
    auto result = control_channel_->Send(data, size, opts);
    if (!result.ok()) return result.error();
    return {};
}

void QuicTransportConnection::SetControlMessageCallback(
    ControlMessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    control_callback_ = std::move(callback);

    control_channel_->SetReceiveCallback(
        [this](const uint8_t* data, size_t size) {
            ControlMessageCallback cb;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cb = control_callback_;
            }
            if (cb) cb(data, size);
        });
}

void QuicTransportConnection::Close() {
    if (!connection_) return;

    // Initiate graceful shutdown. msquic will fire SHUTDOWN_COMPLETE
    // asynchronously, where we call ConnectionClose.
    msquic_->ConnectionShutdown(
        connection_, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
}

std::string QuicTransportConnection::GetRemoteAddress() const {
    return remote_address_;
}

void QuicTransportConnection::SetConnectCallback(
    std::function<void(std::shared_ptr<TransportConnection>)> callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    connect_callback_ = std::move(callback);
}

QUIC_STATUS QuicTransportConnection::SendDatagram(QuicSendContext* ctx) {
    if (!connection_) return QUIC_STATUS_INVALID_STATE;
    return msquic_->DatagramSend(
        connection_, &ctx->buffer, 1, QUIC_SEND_FLAG_NONE, ctx);
}

void QuicTransportConnection::SetState(ConnectionState state) {
    auto prev = state_.exchange(state);
    if (prev == state) return;

    ConnectionStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = state_callback_;
    }
    if (cb) cb(state);
}

// ---------------------------------------------------------------------------
// msquic callbacks (static, dispatch to instance)
// ---------------------------------------------------------------------------

QUIC_STATUS QUIC_API QuicTransportConnection::ConnectionCallback(
    HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event) {
    auto* self = static_cast<QuicTransportConnection*>(context);
    if (!self) return QUIC_STATUS_INVALID_PARAMETER;

    switch (event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        self->OnConnected();
        break;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        self->OnPeerStreamStarted(
            event->PEER_STREAM_STARTED.Stream,
            event->PEER_STREAM_STARTED.Flags);
        break;

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        self->OnDatagramReceived(event->DATAGRAM_RECEIVED.Buffer);
        break;

    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
        self->OnDatagramStateChanged(
            event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE,
            event->DATAGRAM_STATE_CHANGED.MaxSendLength);
        break;

    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        self->OnDatagramSendStateChanged(
            event->DATAGRAM_SEND_STATE_CHANGED.ClientContext,
            event->DATAGRAM_SEND_STATE_CHANGED.State);
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        self->OnShutdownInitiated();
        break;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        self->OnShutdownComplete(
            event->SHUTDOWN_COMPLETE.AppCloseInProgress != FALSE);
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API QuicTransportConnection::StreamRouterCallback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    auto* self = static_cast<QuicTransportConnection*>(context);
    if (!self) return QUIC_STATUS_INVALID_PARAMETER;

    switch (event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        self->HandleStreamReceive(
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            event->RECEIVE.TotalBufferLength);
        break;

    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* ctx = static_cast<QuicSendContext*>(
            event->SEND_COMPLETE.ClientContext);
        delete ctx;
        break;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        // Peer finished sending on this stream — normal for unidirectional.
        break;

    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        break;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (!event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
            self->msquic_->StreamClose(stream);
        }
        // Remove from stream map.
        {
            std::lock_guard<std::mutex> lock(self->stream_map_mutex_);
            self->stream_to_channel_.erase(stream);
        }
        break;

    default:
        break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Connection event handlers
// ---------------------------------------------------------------------------

void QuicTransportConnection::OnConnected() {
    SetState(ConnectionState::kConnected);

    // Client side: open streams for video and control channels.
    if (!is_server_) {
        OpenClientStreams();
    }

    // Fire the connect callback (if set by client/server).
    std::function<void(std::shared_ptr<TransportConnection>)> cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = std::move(connect_callback_);
        connect_callback_ = nullptr;
    }
    if (cb) cb(shared_from_this());
}

void QuicTransportConnection::OnPeerStreamStarted(
    HQUIC stream, QUIC_STREAM_OPEN_FLAGS /*flags*/) {
    // Must set callback handler before returning.
    msquic_->SetCallbackHandler(
        stream, reinterpret_cast<void*>(StreamRouterCallback), this);
    // Stream is unassigned — first RECEIVE will contain the channel_id byte.
}

void QuicTransportConnection::OnDatagramReceived(const QUIC_BUFFER* buffer) {
    if (!buffer || buffer->Length == 0) return;

    // All datagrams go to the audio channel.
    audio_channel_->OnDatagramReceived(buffer->Buffer, buffer->Length);

    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.bytes_received += buffer->Length;
    }
}

void QuicTransportConnection::OnDatagramStateChanged(bool send_enabled,
                                                      uint16_t max_send_length) {
    if (send_enabled) {
        audio_channel_->SetMaxDatagramSize(max_send_length);
    }
}

void QuicTransportConnection::OnDatagramSendStateChanged(
    void* context, QUIC_DATAGRAM_SEND_STATE state) {
    auto* ctx = static_cast<QuicSendContext*>(context);
    // Free the send context when the datagram is no longer in flight.
    switch (state) {
    case QUIC_DATAGRAM_SEND_SENT:
    case QUIC_DATAGRAM_SEND_LOST_DISCARDED:
    case QUIC_DATAGRAM_SEND_ACKNOWLEDGED:
    case QUIC_DATAGRAM_SEND_ACKNOWLEDGED_SPURIOUS:
    case QUIC_DATAGRAM_SEND_CANCELED:
        delete ctx;
        break;
    case QUIC_DATAGRAM_SEND_LOST_SUSPECT:
    case QUIC_DATAGRAM_SEND_UNKNOWN:
        // Still in flight — don't free yet.
        break;
    }
}

void QuicTransportConnection::OnShutdownInitiated() {
    SetState(ConnectionState::kFailed);
}

void QuicTransportConnection::OnShutdownComplete(bool app_close_in_progress) {
    if (!app_close_in_progress && connection_) {
        msquic_->ConnectionClose(connection_);
    }
    connection_ = nullptr;
    SetState(ConnectionState::kDisconnected);
}

// ---------------------------------------------------------------------------
// Stream management
// ---------------------------------------------------------------------------

void QuicTransportConnection::OpenClientStreams() {
    auto open_stream = [&](QuicChannel* channel) {
        HQUIC stream = nullptr;
        QUIC_STATUS status = msquic_->StreamOpen(
            connection_, QUIC_STREAM_OPEN_FLAG_NONE,
            StreamRouterCallback, this, &stream);
        if (QUIC_FAILED(status)) return;

        status = msquic_->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
        if (QUIC_FAILED(status)) {
            msquic_->StreamClose(stream);
            return;
        }

        channel->SetStream(stream);
        {
            std::lock_guard<std::mutex> lock(stream_map_mutex_);
            stream_to_channel_[stream] = channel;
        }

        // Send channel_id as the first byte on the stream.
        auto* ctx = new QuicSendContext();
        ctx->data = {static_cast<uint8_t>(channel->GetType())};
        ctx->Prepare();
        msquic_->StreamSend(stream, &ctx->buffer, 1,
                            QUIC_SEND_FLAG_NONE, ctx);
    };

    open_stream(video_channel_.get());
    open_stream(control_channel_.get());
}

QuicChannel* QuicTransportConnection::FindChannelForStream(HQUIC stream) {
    std::lock_guard<std::mutex> lock(stream_map_mutex_);
    auto it = stream_to_channel_.find(stream);
    return (it != stream_to_channel_.end()) ? it->second : nullptr;
}

void QuicTransportConnection::HandleStreamReceive(
    HQUIC stream, const QUIC_BUFFER* buffers, uint32_t buffer_count,
    uint64_t /*total_length*/) {
    auto* channel = FindChannelForStream(stream);
    if (channel) {
        channel->OnStreamDataReceived(buffers, buffer_count);
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            for (uint32_t i = 0; i < buffer_count; i++) {
                stats_.bytes_received += buffers[i].Length;
            }
        }
    } else {
        // Stream not yet assigned — read channel_id from first byte.
        HandleUnassignedStreamData(stream, buffers, buffer_count);
    }
}

void QuicTransportConnection::HandleUnassignedStreamData(
    HQUIC stream, const QUIC_BUFFER* buffers, uint32_t buffer_count) {
    if (buffer_count == 0 || buffers[0].Length == 0) return;

    uint8_t channel_id = buffers[0].Buffer[0];
    auto* channel = ChannelByType(static_cast<ChannelType>(channel_id));
    if (!channel || channel->GetMode() != QuicChannel::Mode::kStream) return;

    channel->SetStream(stream);
    {
        std::lock_guard<std::mutex> lock(stream_map_mutex_);
        stream_to_channel_[stream] = channel;
    }

    // Forward remaining data (everything after the channel_id byte).
    // Build a contiguous buffer of remaining bytes.
    std::vector<uint8_t> remaining;
    bool first = true;
    for (uint32_t i = 0; i < buffer_count; i++) {
        const uint8_t* buf = buffers[i].Buffer;
        uint32_t len = buffers[i].Length;
        if (first) {
            buf += 1;
            len -= 1;
            first = false;
        }
        if (len > 0) {
            remaining.insert(remaining.end(), buf, buf + len);
        }
    }
    if (!remaining.empty()) {
        QUIC_BUFFER qbuf;
        qbuf.Buffer = remaining.data();
        qbuf.Length = static_cast<uint32_t>(remaining.size());
        channel->OnStreamDataReceived(&qbuf, 1);
    }
}

}  // namespace lumen
