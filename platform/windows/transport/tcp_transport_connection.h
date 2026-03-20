#pragma once

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"
#include "tcp_transport_channel.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace lumen {

/// TCP-based transport connection.
///
/// Owns a SOCKET, three logical channels (video, audio, control), and an I/O
/// thread. The I/O thread uses select() to send from a priority queue and
/// receive framed data. Wire format per message: [channel_id:u8][length:u32][payload].
class TcpTransportConnection
    : public TransportConnection,
      public std::enable_shared_from_this<TcpTransportConnection> {
public:
    /// Construct around an already-connected socket.
    TcpTransportConnection(SOCKET socket, const std::string& remote_address,
                           size_t max_payload_size = 1200);
    ~TcpTransportConnection() override;

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

    /// Enqueue data for sending on the I/O thread (called by channels).
    /// Returns false if the queue is full.
    bool EnqueueSend(uint8_t channel_id, std::vector<uint8_t> payload,
                     SendPriority priority);

    /// Available capacity in the send queue.
    size_t GetSendQueueCapacity() const;

    /// Notify channels that send capacity is available.
    void NotifyReadyToSend();

    /// Start the I/O thread. Must be called after construction.
    void Start();

private:
    static constexpr size_t kFrameHeaderSize = 5;  // 1 byte channel + 4 bytes length
    static constexpr size_t kMaxSendQueueBytes = 4 * 1024 * 1024;
    static constexpr size_t kMaxRecvPayloadBytes = 2 * 1024 * 1024;

    struct QueueEntry {
        uint8_t channel_id;
        std::vector<uint8_t> payload;
        SendPriority priority;
        uint64_t sequence;  // For stable ordering within same priority

        bool operator>(const QueueEntry& other) const {
            if (priority != other.priority)
                return static_cast<uint8_t>(priority) >
                       static_cast<uint8_t>(other.priority);
            return sequence > other.sequence;
        }
    };

    // Comparator for the heap (min-heap: highest priority = lowest enum value first).
    struct HeapCompare {
        bool operator()(const QueueEntry& a, const QueueEntry& b) const {
            return a > b;  // std::push_heap is a max-heap, so invert for min-heap
        }
    };

    void IOThreadFunc();
    void SetState(ConnectionState state);
    TcpTransportChannel* ChannelById(uint8_t id);

    SOCKET socket_;
    std::string remote_address_;
    std::atomic<ConnectionState> state_{ConnectionState::kConnected};

    std::unique_ptr<TcpTransportChannel> video_channel_;
    std::unique_ptr<TcpTransportChannel> audio_channel_;
    std::unique_ptr<TcpTransportChannel> control_channel_;

    // I/O thread
    std::thread io_thread_;
    std::atomic<bool> stop_requested_{false};

    // Priority send queue (vector-based heap for proper extraction)
    mutable std::mutex send_mutex_;
    std::condition_variable send_cv_;
    std::vector<QueueEntry> send_heap_;
    size_t send_queue_bytes_ = 0;
    uint64_t send_sequence_ = 0;

    // Monotonic frame index for control messages
    std::atomic<uint64_t> control_frame_index_{0};

    // Partial send state (non-blocking I/O)
    std::vector<uint8_t> pending_send_buf_;
    size_t pending_send_offset_ = 0;

    // Recv state machine (non-blocking I/O)
    enum class RecvState { kReadingHeader, kReadingPayload };
    RecvState recv_state_ = RecvState::kReadingHeader;
    uint8_t recv_header_buf_[kFrameHeaderSize] = {};
    size_t recv_header_offset_ = 0;
    std::vector<uint8_t> recv_payload_buf_;
    size_t recv_payload_offset_ = 0;
    uint8_t recv_channel_id_ = 0;

    // Stats
    mutable std::mutex stats_mutex_;
    TransportStats stats_;

    // Callbacks
    mutable std::mutex callback_mutex_;
    ConnectionStateCallback state_callback_;
    StatsCallback stats_callback_;
    ControlMessageCallback control_callback_;
};

}  // namespace lumen
