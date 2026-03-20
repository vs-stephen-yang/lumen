#pragma once

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_types.h"
#include "frame_fragmenter.h"
#include "frame_reassembler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <msquic.h>

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace lumen {

class QuicTransportConnection;

/// Heap-allocated send context — freed in SEND_COMPLETE or DATAGRAM_SEND_STATE_CHANGED.
struct QuicSendContext {
    std::vector<uint8_t> data;
    QUIC_BUFFER buffer;

    void Prepare() {
        buffer.Buffer = data.data();
        buffer.Length = static_cast<uint32_t>(data.size());
    }
};

/// QUIC-based transport channel.
///
/// Operates in one of two modes:
/// - Stream mode (video, control): length-prefixed messages on a persistent
///   bidirectional QUIC stream. Wire format per message:
///   [length:u32 big-endian][MediaPacketHeader:28][frame payload]
/// - Datagram mode (audio): MTU-sized QUIC datagrams with fragmenter/reassembler.
///   Wire format per datagram: [MediaPacketHeader:28][fragment payload]
class QuicChannel : public TransportChannel {
public:
    enum class Mode { kStream, kDatagram };

    QuicChannel(ChannelType type, QuicTransportConnection* connection,
                Mode mode, const QUIC_API_TABLE* msquic);
    ~QuicChannel() override;

    // TransportChannel interface
    Result<size_t> Send(const uint8_t* data, size_t size,
                        const SendOptions& options) override;
    void SetReceiveCallback(DataReceivedCallback callback) override;
    size_t GetSendCapacity() const override;
    void SetReadyToSendCallback(ReadyToSendCallback callback) override;
    ChannelType GetType() const override { return type_; }
    ReliabilityMode GetReliability() const override;
    void SetPriority(SendPriority priority) override;
    size_t GetMaxPayloadSize() const override;

    Mode GetMode() const { return mode_; }

    /// Set the QUIC stream handle (called after stream is opened/accepted).
    void SetStream(HQUIC stream);
    HQUIC GetStream() const { return stream_; }

    /// Called when stream data arrives (from connection's stream callback).
    void OnStreamDataReceived(const QUIC_BUFFER* buffers, uint32_t buffer_count);

    /// Called when a datagram arrives (from connection's datagram callback).
    void OnDatagramReceived(const uint8_t* data, size_t size);

    /// Update max datagram payload size (from DATAGRAM_STATE_CHANGED).
    void SetMaxDatagramSize(uint16_t size);

    /// Fire the ready-to-send callback (called by connection after send completes).
    void FireReadyToSend();

private:
    static constexpr size_t kStreamFrameHeaderSize = 4;  // u32 length prefix
    static constexpr size_t kMaxRecvMessageSize = 2 * 1024 * 1024;

    SendPriority DefaultPriority() const;
    Result<size_t> SendOnStream(const uint8_t* data, size_t size,
                                 const SendOptions& options);
    Result<size_t> SendOnDatagram(const uint8_t* data, size_t size,
                                   const SendOptions& options);
    void ProcessStreamRecvBuffer();

    ChannelType type_;
    Mode mode_;
    QuicTransportConnection* connection_;
    const QUIC_API_TABLE* msquic_;
    HQUIC stream_ = nullptr;
    SendPriority priority_;

    // Datagram-mode fragmentation
    FrameFragmenter fragmenter_;
    FrameReassembler reassembler_;
    uint16_t max_datagram_size_ = 1200;

    // Stream-mode recv buffer (accumulates bytes for length-prefixed parsing)
    std::vector<uint8_t> stream_recv_buf_;

    mutable std::mutex mutex_;
    DataReceivedCallback receive_callback_;
    ReadyToSendCallback ready_to_send_callback_;
};

}  // namespace lumen
