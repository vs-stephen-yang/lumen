#pragma once

#include "lumen/capture/audio_capture.h"

#include <vector>

namespace lumen {

/// Mock audio capture for unit testing.
/// Returns pre-configured audio frames without any audio hardware.
class MockAudioCapture : public AudioCapture {
public:
    Result<void> Initialize() override {
        initialized_ = true;
        return {};
    }

    Result<void> Start(AudioFrameCallback callback) override {
        callback_ = std::move(callback);
        return {};
    }

    Result<void> Stop() override {
        callback_ = nullptr;
        return {};
    }

    AudioFormat GetFormat() const override { return format_; }

    // Test helpers
    void SetFormat(AudioFormat format) { format_ = format; }
    bool IsInitialized() const { return initialized_; }

    /// Deliver a frame to the registered callback.
    void DeliverFrame(const AudioFrame& frame) {
        if (callback_) callback_(frame);
    }

private:
    bool initialized_ = false;
    AudioFormat format_ = {48000, 2, AudioSampleFormat::kFloat32};
    AudioFrameCallback callback_;
};

}  // namespace lumen
