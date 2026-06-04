#pragma once

#include "lumen/codec/video_encoder.h"

#include <optional>
#include <vector>

namespace lumen {

/// Mock video encoder for unit testing.
/// Records Encode() calls and produces dummy encoded packets.
class MockVideoEncoder : public VideoEncoder {
public:
    Result<void> Initialize(const VideoEncoderConfig& config,
                            void* /*device*/) override {
        config_ = config;
        initialized_ = true;
        return {};
    }

    Result<void> Encode(void* /*native_texture*/,
                        const FrameMetadata& metadata) override {
        encode_count_++;

        if (encode_error_) return Error::Make(*encode_error_, "mock encode");

        if (output_callback_) {
            EncodedPacket packet;
            packet.data = {0x00, 0x00, 0x00, 0x01};  // Dummy NAL start code
            packet.metadata = metadata;
            packet.metadata.is_keyframe = keyframe_requested_;
            keyframe_requested_ = false;
            output_callback_(std::move(packet));
        }

        return {};
    }

    Result<void> Flush() override { return {}; }

    void SetOutputCallback(EncodedPacketCallback callback) override {
        output_callback_ = std::move(callback);
    }

    void RequestKeyframe() override { keyframe_requested_ = true; }

    Result<void> SetBitrate(uint32_t bitrate_bps) override {
        config_.bitrate_bps = bitrate_bps;
        return {};
    }

    Result<void> SetFrameRate(uint32_t fps) override {
        config_.fps = fps;
        return {};
    }

    Result<void> Reconfigure(const VideoEncoderConfig& new_config) override {
        config_ = new_config;
        return {};
    }

    VideoCodec GetCodec() const override { return config_.codec; }

    // Test helpers
    void FailEncodeWith(ErrorCode c) { encode_error_ = c; }
    bool IsInitialized() const { return initialized_; }
    uint32_t EncodeCount() const { return encode_count_; }

private:
    bool initialized_ = false;
    bool keyframe_requested_ = false;
    uint32_t encode_count_ = 0;
    VideoEncoderConfig config_;
    EncodedPacketCallback output_callback_;
    std::optional<ErrorCode> encode_error_;
};

}  // namespace lumen
