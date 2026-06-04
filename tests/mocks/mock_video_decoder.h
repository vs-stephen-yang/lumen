#pragma once

#include "lumen/codec/video_decoder.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lumen {

/// Mock video decoder for unit testing.
/// Produces synthetic DecodedFrames (no real GPU texture) and supports error
/// injection so decode error paths can be exercised without hardware.
class MockVideoDecoder : public VideoDecoder {
public:
    Result<void> Initialize(const VideoDecoderConfig& config,
                            void* /*device*/) override {
        if (init_error_) return Error::Make(*init_error_, "mock init");
        config_ = config;
        initialized_ = true;
        return {};
    }

    Result<DecodedFrame> Decode(const uint8_t* /*data*/, size_t size,
                                uint64_t frame_index) override {
        decode_count_++;
        last_decode_size_ = size;
        if (decode_error_) return Error::Make(*decode_error_, "mock decode");
        if (need_more_data_) return Error::Make(ErrorCode::kTimeout);

        DecodedFrame f;
        f.native_texture = nullptr;  // synthetic — no GPU surface
        f.width = config_.width;
        f.height = config_.height;
        f.format = PixelFormat::kNV12;
        f.frame_index = frame_index;
        return f;
    }

    Result<void> Flush() override { return {}; }
    void ReleaseFrame(const DecodedFrame& /*frame*/) override {
        release_count_++;
    }
    VideoCodec GetCodec() const override { return config_.codec; }

    // ── Test controls ───────────────────────────────────────────────
    void FailInitializeWith(ErrorCode c) { init_error_ = c; }
    void FailDecodeWith(ErrorCode c) { decode_error_ = c; }
    /// Make Decode() return kTimeout (decoder wants more input).
    void SetNeedMoreData(bool v) { need_more_data_ = v; }

    bool IsInitialized() const { return initialized_; }
    uint32_t DecodeCount() const { return decode_count_; }
    uint32_t ReleaseCount() const { return release_count_; }
    size_t LastDecodeSize() const { return last_decode_size_; }

private:
    bool initialized_ = false;
    VideoDecoderConfig config_;
    uint32_t decode_count_ = 0;
    uint32_t release_count_ = 0;
    size_t last_decode_size_ = 0;
    bool need_more_data_ = false;
    std::optional<ErrorCode> init_error_;
    std::optional<ErrorCode> decode_error_;
};

}  // namespace lumen
