#pragma once

#include "lumen/capture/screen_capture.h"

#include <vector>

namespace lumen {

/// Mock screen capture for unit testing.
/// Returns pre-configured frames without any GPU interaction.
class MockScreenCapture : public ScreenCapture {
public:
    Result<void> Initialize(uint32_t /*output_index*/) override {
        initialized_ = true;
        return {};
    }

    Result<void> Start(FrameCallback callback) override {
        callback_ = std::move(callback);
        return {};
    }

    Result<void> Stop() override {
        callback_ = nullptr;
        return {};
    }

    Result<CapturedFrame> AcquireFrame(uint32_t /*timeout_ms*/) override {
        if (frames_to_deliver_.empty()) {
            return Error::Make(ErrorCode::kTimeout, "No frames queued");
        }
        auto frame = frames_to_deliver_.front();
        frames_to_deliver_.erase(frames_to_deliver_.begin());
        return frame;
    }

    void ReleaseFrame() override {}

    VideoFrameDesc GetFrameDesc() const override { return desc_; }

    // Test helpers
    void SetFrameDesc(VideoFrameDesc desc) { desc_ = desc; }
    void QueueFrame(CapturedFrame frame) { frames_to_deliver_.push_back(frame); }
    bool IsInitialized() const { return initialized_; }

private:
    bool initialized_ = false;
    VideoFrameDesc desc_;
    FrameCallback callback_;
    std::vector<CapturedFrame> frames_to_deliver_;
};

}  // namespace lumen
