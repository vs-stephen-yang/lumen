#pragma once

#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"
#include "mock_transport_channel.h"

#include <memory>
#include <mutex>
#include <string>

namespace lumen {

/// Mock transport connection for unit testing.
/// Owns three MockTransportChannels and exposes SetState()/SetStats().
class MockTransportConnection : public TransportConnection {
public:
    MockTransportConnection()
        : video_(std::make_unique<MockTransportChannel>(ChannelType::kVideo)),
          audio_(std::make_unique<MockTransportChannel>(ChannelType::kAudio)),
          control_(std::make_unique<MockTransportChannel>(ChannelType::kControl)) {}

    TransportChannel* GetChannel(ChannelType type) override {
        switch (type) {
            case ChannelType::kVideo:   return video_.get();
            case ChannelType::kAudio:   return audio_.get();
            case ChannelType::kControl: return control_.get();
        }
        return nullptr;
    }

    ConnectionState GetState() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    void SetStateCallback(ConnectionStateCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        state_callback_ = std::move(callback);
    }

    TransportStats GetStats() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_;
    }

    void SetStatsCallback(StatsCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_callback_ = std::move(callback);
    }

    Result<void> RequestKeyframe() override {
        std::lock_guard<std::mutex> lock(mutex_);
        keyframe_request_count_++;
        return {};
    }

    Result<void> SendControlMessage(const uint8_t* data,
                                    size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        control_messages_.emplace_back(data, data + size);
        return {};
    }

    void SetControlMessageCallback(ControlMessageCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        control_callback_ = std::move(callback);
    }

    void Close() override {
        SetState(ConnectionState::kDisconnected);
    }

    std::string GetRemoteAddress() const override {
        return "mock:0";
    }

    // --- Test helpers ---

    void SetState(ConnectionState state) {
        ConnectionStateCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = state;
            cb = state_callback_;
        }
        if (cb) cb(state);
    }

    void SetStats(const TransportStats& stats) {
        StatsCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_ = stats;
            cb = stats_callback_;
        }
        if (cb) cb(stats);
    }

    uint32_t GetKeyframeRequestCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return keyframe_request_count_;
    }

    MockTransportChannel* GetMockChannel(ChannelType type) {
        switch (type) {
            case ChannelType::kVideo:   return video_.get();
            case ChannelType::kAudio:   return audio_.get();
            case ChannelType::kControl: return control_.get();
        }
        return nullptr;
    }

private:
    mutable std::mutex mutex_;
    ConnectionState state_ = ConnectionState::kDisconnected;
    TransportStats stats_;
    uint32_t keyframe_request_count_ = 0;
    std::vector<std::vector<uint8_t>> control_messages_;

    std::unique_ptr<MockTransportChannel> video_;
    std::unique_ptr<MockTransportChannel> audio_;
    std::unique_ptr<MockTransportChannel> control_;

    ConnectionStateCallback state_callback_;
    StatsCallback stats_callback_;
    ControlMessageCallback control_callback_;
};

}  // namespace lumen
