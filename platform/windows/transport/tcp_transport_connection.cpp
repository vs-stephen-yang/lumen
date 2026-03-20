#include "tcp_transport_connection.h"

#include <algorithm>
#include <cstring>

namespace lumen {

TcpTransportConnection::TcpTransportConnection(SOCKET socket,
                                               const std::string& remote_address,
                                               size_t /*max_payload_size*/)
    : socket_(socket), remote_address_(remote_address) {
    video_channel_ = std::make_unique<TcpTransportChannel>(
        ChannelType::kVideo, this);
    audio_channel_ = std::make_unique<TcpTransportChannel>(
        ChannelType::kAudio, this);
    control_channel_ = std::make_unique<TcpTransportChannel>(
        ChannelType::kControl, this);
}

TcpTransportConnection::~TcpTransportConnection() {
    Close();
}

void TcpTransportConnection::Start() {
    io_thread_ = std::thread(&TcpTransportConnection::IOThreadFunc, this);
}

TransportChannel* TcpTransportConnection::GetChannel(ChannelType type) {
    switch (type) {
        case ChannelType::kVideo:   return video_channel_.get();
        case ChannelType::kAudio:   return audio_channel_.get();
        case ChannelType::kControl: return control_channel_.get();
    }
    return nullptr;
}

ConnectionState TcpTransportConnection::GetState() const {
    return state_.load();
}

void TcpTransportConnection::SetStateCallback(ConnectionStateCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    state_callback_ = std::move(callback);
}

TransportStats TcpTransportConnection::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void TcpTransportConnection::SetStatsCallback(StatsCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    stats_callback_ = std::move(callback);
}

Result<void> TcpTransportConnection::RequestKeyframe() {
    static const uint8_t kKeyframeRequest[] = {'K', 'F', 'R', 'Q'};
    return SendControlMessage(kKeyframeRequest, sizeof(kKeyframeRequest));
}

Result<void> TcpTransportConnection::SendControlMessage(const uint8_t* data,
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

void TcpTransportConnection::SetControlMessageCallback(
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

// Step 3: Close socket FIRST to unblock I/O thread, THEN join.
void TcpTransportConnection::Close() {
    stop_requested_ = true;
    send_cv_.notify_all();

    if (socket_ != INVALID_SOCKET) {
        ::shutdown(socket_, SD_BOTH);
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (io_thread_.joinable()) {
        io_thread_.join();
    }

    SetState(ConnectionState::kDisconnected);
}

std::string TcpTransportConnection::GetRemoteAddress() const {
    return remote_address_;
}

// Step 6: Returns false when queue is full.
bool TcpTransportConnection::EnqueueSend(uint8_t channel_id,
                                          std::vector<uint8_t> payload,
                                          SendPriority priority) {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (send_queue_bytes_ + payload.size() > kMaxSendQueueBytes) return false;

    QueueEntry entry;
    entry.channel_id = channel_id;
    entry.priority = priority;
    entry.sequence = send_sequence_++;
    send_queue_bytes_ += payload.size();
    entry.payload = std::move(payload);
    send_heap_.push_back(std::move(entry));
    std::push_heap(send_heap_.begin(), send_heap_.end(), HeapCompare{});
    send_cv_.notify_one();
    return true;
}

size_t TcpTransportConnection::GetSendQueueCapacity() const {
    std::lock_guard<std::mutex> lock(send_mutex_);
    return kMaxSendQueueBytes > send_queue_bytes_
               ? kMaxSendQueueBytes - send_queue_bytes_
               : 0;
}

void TcpTransportConnection::NotifyReadyToSend() {
    if (video_channel_)   video_channel_->FireReadyToSend();
    if (audio_channel_)   audio_channel_->FireReadyToSend();
    if (control_channel_) control_channel_->FireReadyToSend();
}

void TcpTransportConnection::SetState(ConnectionState state) {
    auto prev = state_.exchange(state);
    if (prev == state) return;

    ConnectionStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        cb = state_callback_;
    }
    if (cb) cb(state);
}

TcpTransportChannel* TcpTransportConnection::ChannelById(uint8_t id) {
    switch (static_cast<ChannelType>(id)) {
        case ChannelType::kVideo:   return video_channel_.get();
        case ChannelType::kAudio:   return audio_channel_.get();
        case ChannelType::kControl: return control_channel_.get();
    }
    return nullptr;
}

// Step 5: Fully non-blocking I/O thread with drain loop and recv state machine.
void TcpTransportConnection::IOThreadFunc() {
    SetState(ConnectionState::kConnected);

    // Set socket to non-blocking for the entire lifetime of this thread.
    u_long non_blocking = 1;
    ioctlsocket(socket_, FIONBIO, &non_blocking);

    while (!stop_requested_) {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        if (socket_ == INVALID_SOCKET) break;

        FD_SET(socket_, &read_fds);

        // Check if we have pending data or queued entries to send.
        bool has_pending_send = (pending_send_offset_ < pending_send_buf_.size());
        if (!has_pending_send) {
            std::lock_guard<std::mutex> lock(send_mutex_);
            has_pending_send = !send_heap_.empty();
        }
        if (has_pending_send) {
            FD_SET(socket_, &write_fds);
        }

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 10000;  // 10ms

        int sel = ::select(0, &read_fds, &write_fds, nullptr, &timeout);
        if (sel == SOCKET_ERROR) {
            if (!stop_requested_) SetState(ConnectionState::kFailed);
            break;
        }

        // --- Send path: drain loop ---
        if (FD_ISSET(socket_, &write_fds)) {
            bool send_failed = false;
            bool drained_any = false;

            for (;;) {
                // First, finish any pending partial send.
                while (pending_send_offset_ < pending_send_buf_.size()) {
                    int result = ::send(
                        socket_,
                        reinterpret_cast<const char*>(
                            pending_send_buf_.data() + pending_send_offset_),
                        static_cast<int>(pending_send_buf_.size() -
                                         pending_send_offset_),
                        0);
                    if (result == SOCKET_ERROR) {
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) {
                            goto send_done;  // Socket buffer full, try next iteration
                        }
                        send_failed = true;
                        goto send_done;
                    }
                    pending_send_offset_ += result;
                    {
                        std::lock_guard<std::mutex> lock(stats_mutex_);
                        stats_.bytes_sent += result;
                    }
                }

                // Pending send complete — clear the buffer.
                pending_send_buf_.clear();
                pending_send_offset_ = 0;

                // Dequeue next entry from the heap.
                QueueEntry entry;
                bool got_entry = false;
                {
                    std::lock_guard<std::mutex> lock(send_mutex_);
                    if (!send_heap_.empty()) {
                        std::pop_heap(send_heap_.begin(), send_heap_.end(),
                                      HeapCompare{});
                        entry = std::move(send_heap_.back());
                        send_heap_.pop_back();
                        send_queue_bytes_ -= entry.payload.size();
                        got_entry = true;
                        drained_any = true;
                    }
                }

                if (!got_entry) break;

                // Build wire frame: [channel_id:u8][length:u32][payload]
                size_t wire_size = kFrameHeaderSize + entry.payload.size();
                pending_send_buf_.resize(wire_size);
                pending_send_buf_[0] = entry.channel_id;
                uint32_t len = static_cast<uint32_t>(entry.payload.size());
                pending_send_buf_[1] =
                    static_cast<uint8_t>((len >> 24) & 0xFF);
                pending_send_buf_[2] =
                    static_cast<uint8_t>((len >> 16) & 0xFF);
                pending_send_buf_[3] =
                    static_cast<uint8_t>((len >> 8) & 0xFF);
                pending_send_buf_[4] = static_cast<uint8_t>(len & 0xFF);
                std::memcpy(pending_send_buf_.data() + kFrameHeaderSize,
                            entry.payload.data(), entry.payload.size());
                pending_send_offset_ = 0;
            }

        send_done:
            if (send_failed) {
                if (!stop_requested_) SetState(ConnectionState::kFailed);
                break;
            }
            if (drained_any) {
                NotifyReadyToSend();
            }
        }

        // --- Recv path: non-blocking state machine ---
        if (FD_ISSET(socket_, &read_fds)) {
            bool recv_failed = false;

            for (;;) {
                if (recv_state_ == RecvState::kReadingHeader) {
                    int result = ::recv(
                        socket_,
                        reinterpret_cast<char*>(recv_header_buf_ +
                                                recv_header_offset_),
                        static_cast<int>(kFrameHeaderSize -
                                         recv_header_offset_),
                        0);
                    if (result == SOCKET_ERROR) {
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) break;
                        recv_failed = true;
                        break;
                    }
                    if (result == 0) {
                        // Peer closed connection.
                        recv_failed = true;
                        break;
                    }
                    recv_header_offset_ += result;

                    if (recv_header_offset_ < kFrameHeaderSize) {
                        // Partial header — wait for more data.
                        break;
                    }

                    // Header complete — parse it.
                    recv_channel_id_ = recv_header_buf_[0];
                    uint32_t payload_len =
                        (static_cast<uint32_t>(recv_header_buf_[1]) << 24) |
                        (static_cast<uint32_t>(recv_header_buf_[2]) << 16) |
                        (static_cast<uint32_t>(recv_header_buf_[3]) << 8) |
                        static_cast<uint32_t>(recv_header_buf_[4]);

                    if (payload_len > kMaxRecvPayloadBytes) {
                        recv_failed = true;
                        break;
                    }

                    if (payload_len == 0) {
                        // Empty payload — dispatch and reset.
                        auto* channel = ChannelById(recv_channel_id_);
                        if (channel) {
                            channel->OnDataReceived(nullptr, 0);
                        }
                        recv_header_offset_ = 0;
                        continue;
                    }

                    recv_payload_buf_.resize(payload_len);
                    recv_payload_offset_ = 0;
                    recv_state_ = RecvState::kReadingPayload;
                }

                if (recv_state_ == RecvState::kReadingPayload) {
                    size_t remaining =
                        recv_payload_buf_.size() - recv_payload_offset_;
                    int result = ::recv(
                        socket_,
                        reinterpret_cast<char*>(recv_payload_buf_.data() +
                                                recv_payload_offset_),
                        static_cast<int>(remaining), 0);
                    if (result == SOCKET_ERROR) {
                        int err = WSAGetLastError();
                        if (err == WSAEWOULDBLOCK) break;
                        recv_failed = true;
                        break;
                    }
                    if (result == 0) {
                        recv_failed = true;
                        break;
                    }
                    recv_payload_offset_ += result;
                    {
                        std::lock_guard<std::mutex> lock(stats_mutex_);
                        stats_.bytes_received += result;
                    }

                    if (recv_payload_offset_ < recv_payload_buf_.size()) {
                        // Partial payload — wait for more data.
                        break;
                    }

                    // Payload complete — dispatch to channel.
                    auto* channel = ChannelById(recv_channel_id_);
                    if (channel) {
                        channel->OnDataReceived(recv_payload_buf_.data(),
                                                recv_payload_buf_.size());
                    }

                    // Reset for next message.
                    recv_state_ = RecvState::kReadingHeader;
                    recv_header_offset_ = 0;
                }
            }

            if (recv_failed) {
                if (!stop_requested_) SetState(ConnectionState::kFailed);
                break;
            }
        }
    }
}

}  // namespace lumen
