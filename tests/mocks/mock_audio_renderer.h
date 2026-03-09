#pragma once

#include "lumen/renderer/audio_renderer.h"

#include <vector>

namespace lumen {

/// Mock audio renderer for unit testing.
/// Records queued frames and tracks statistics.
class MockAudioRenderer : public AudioRenderer {
public:
    Result<void> Initialize(const AudioRendererConfig& config) override {
        config_ = config;
        initialized_ = true;
        return {};
    }

    Result<void> Start() override {
        running_ = true;
        return {};
    }

    Result<void> Stop() override {
        running_ = false;
        return {};
    }

    Result<void> QueueFrame(const DecodedAudioFrame& frame) override {
        frames_queued_++;
        total_samples_ += frame.frame_count;
        last_timestamp_us_ = frame.timestamp_us;
        return {};
    }

    AudioRendererStats GetStats() const override {
        AudioRendererStats stats;
        stats.frames_played = frames_queued_;
        stats.buffer_level_ms = 20.0;
        return stats;
    }

    Timestamp GetPlaybackTimestamp() const override {
        return last_timestamp_us_;
    }

    // Test helpers
    bool IsInitialized() const { return initialized_; }
    bool IsRunning() const { return running_; }
    uint32_t FramesQueued() const { return frames_queued_; }
    uint64_t TotalSamples() const { return total_samples_; }

private:
    bool initialized_ = false;
    bool running_ = false;
    uint32_t frames_queued_ = 0;
    uint64_t total_samples_ = 0;
    Timestamp last_timestamp_us_ = 0;
    AudioRendererConfig config_;
};

}  // namespace lumen
