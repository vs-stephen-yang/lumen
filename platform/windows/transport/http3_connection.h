#pragma once

// HTTP/3 connection skeleton — sits between an msquic connection and the
// nghttp3 HTTP/3 layer (Phase 2.1). For 2.0 we only track lifecycle and
// log stream events; nghttp3 callbacks are wired in 2.1.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msquic.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace lumen {

/// One HTTP/3 connection riding on a single QUIC connection.
class Http3Connection {
public:
    Http3Connection(const QUIC_API_TABLE* msquic, HQUIC connection,
                     std::string remote);
    ~Http3Connection();

    /// Called by Http3Server right after construction so msquic events
    /// for this connection route here.
    void AttachCallback();

    HQUIC handle() const { return handle_; }
    const std::string& remote_address() const { return remote_address_; }

    /// Number of QUIC streams seen so far (debug aid).
    uint64_t streams_seen() const { return streams_seen_.load(); }

    /// True once TLS is up and the connection is usable.
    bool connected() const { return connected_.load(); }

    /// Initiate graceful shutdown.
    void Close();

private:
    static QUIC_STATUS QUIC_API ConnectionCallback(
        HQUIC connection, void* context, QUIC_CONNECTION_EVENT* event);
    static QUIC_STATUS QUIC_API StreamCallback(
        HQUIC stream, void* context, QUIC_STREAM_EVENT* event);

    QUIC_STATUS HandleConnectionEvent(QUIC_CONNECTION_EVENT* event);
    QUIC_STATUS HandleStreamEvent(HQUIC stream, QUIC_STREAM_EVENT* event);

    const QUIC_API_TABLE* msquic_ = nullptr;
    HQUIC handle_ = nullptr;
    std::string remote_address_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> streams_seen_{0};
};

}  // namespace lumen
