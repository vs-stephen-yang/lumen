#pragma once

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_types.h"

#include <mutex>
#include <optional>
#include <vector>

namespace lumen {

/// Mock transport channel for unit testing.
/// Records sent data and exposes SimulateReceive() for injecting data.
class MockTransportChannel : public TransportChannel {
public:
    explicit MockTransportChannel(ChannelType type)
        : type_(type), priority_(SendPriority::kBulk) {}

    Result<size_t> Send(const uint8_t* data, size_t size,
                        const SendOptions& options) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (send_error_) return Error::Make(*send_error_, "mock send");
        sent_data_.emplace_back(data, data + size);
        sent_options_.push_back(options);
        return size;
    }

    void SetReceiveCallback(DataReceivedCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        receive_callback_ = std::move(callback);
    }

    size_t GetSendCapacity() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return send_capacity_;
    }

    void SetReadyToSendCallback(ReadyToSendCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_callback_ = std::move(callback);
    }

    ChannelType GetType() const override { return type_; }

    ReliabilityMode GetReliability() const override {
        return ReliabilityMode::kReliableOrdered;
    }

    void SetPriority(SendPriority priority) override {
        std::lock_guard<std::mutex> lock(mutex_);
        priority_ = priority;
    }

    size_t GetMaxPayloadSize() const override { return 1200; }

    // --- Test helpers ---

    /// Simulate receiving data (fires the receive callback).
    void SimulateReceive(const uint8_t* data, size_t size) {
        DataReceivedCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cb = receive_callback_;
        }
        if (cb) cb(data, size);
    }

    /// Get all sent data buffers.
    std::vector<std::vector<uint8_t>> GetSentData() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_data_;
    }

    /// Get all send options.
    std::vector<SendOptions> GetSentOptions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_options_;
    }

    size_t SendCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_data_.size();
    }

    /// Make Send() fail with `c` (e.g. kTransportChannelFull).
    void FailSendWith(ErrorCode c) {
        std::lock_guard<std::mutex> lock(mutex_);
        send_error_ = c;
    }

    /// Override the reported send capacity (0 simulates backpressure).
    void SetSendCapacity(size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        send_capacity_ = bytes;
    }

private:
    ChannelType type_;
    SendPriority priority_;
    mutable std::mutex mutex_;
    std::vector<std::vector<uint8_t>> sent_data_;
    std::vector<SendOptions> sent_options_;
    DataReceivedCallback receive_callback_;
    ReadyToSendCallback ready_callback_;
    std::optional<ErrorCode> send_error_;
    size_t send_capacity_ = 1024 * 1024;
};

}  // namespace lumen
