#include "http3_connection.h"

#include <cstdio>

namespace lumen {

Http3Connection::Http3Connection(const QUIC_API_TABLE* msquic,
                                   HQUIC connection, std::string remote)
    : msquic_(msquic), handle_(connection), remote_address_(std::move(remote)) {}

Http3Connection::~Http3Connection() { Close(); }

void Http3Connection::AttachCallback() {
    msquic_->SetCallbackHandler(
        handle_, reinterpret_cast<void*>(&Http3Connection::ConnectionCallback),
        this);
}

void Http3Connection::Close() {
    if (closed_.exchange(true)) return;
    if (handle_) {
        msquic_->ConnectionShutdown(handle_,
                                     QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    }
}

QUIC_STATUS QUIC_API Http3Connection::ConnectionCallback(
    HQUIC /*connection*/, void* context, QUIC_CONNECTION_EVENT* event) {
    return static_cast<Http3Connection*>(context)->HandleConnectionEvent(event);
}

QUIC_STATUS Http3Connection::HandleConnectionEvent(
    QUIC_CONNECTION_EVENT* event) {
    switch (event->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED:
            connected_.store(true);
            // Phase 2.1 will: open uni control stream, uni QPACK encoder,
            // uni QPACK decoder, and emit SETTINGS frame on the control
            // stream. For 2.0 we just observe the connection succeeding.
            break;

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
            connected_.store(false);
            if (event->Type == QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE &&
                !event->SHUTDOWN_COMPLETE.AppCloseInProgress) {
                msquic_->ConnectionClose(handle_);
                handle_ = nullptr;
            }
            break;

        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            // For 2.0 we just count and accept; the stream is owned by
            // msquic until we set a callback on it.
            streams_seen_.fetch_add(1);
            HQUIC stream = event->PEER_STREAM_STARTED.Stream;
            msquic_->SetCallbackHandler(
                stream,
                reinterpret_cast<void*>(&Http3Connection::StreamCallback),
                this);
            break;
        }

        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API Http3Connection::StreamCallback(
    HQUIC stream, void* context, QUIC_STREAM_EVENT* event) {
    return static_cast<Http3Connection*>(context)->HandleStreamEvent(stream,
                                                                        event);
}

QUIC_STATUS Http3Connection::HandleStreamEvent(HQUIC stream,
                                                 QUIC_STREAM_EVENT* event) {
    switch (event->Type) {
        case QUIC_STREAM_EVENT_RECEIVE:
            // Phase 2.1: forward bytes into nghttp3_conn_read_stream().
            // For 2.0, just acknowledge so flow control opens.
            break;

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            // Peer is done sending on this stream.
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
            msquic_->StreamClose(stream);
            break;

        default:
            break;
    }
    return QUIC_STATUS_SUCCESS;
}

}  // namespace lumen
