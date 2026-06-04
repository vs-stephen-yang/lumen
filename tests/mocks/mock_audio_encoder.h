#pragma once

#include "lumen/codec/audio_encoder.h"

#include <optional>
#include <vector>

namespace lumen {

/// Mock audio encoder for unit testing.
/// Records Encode() calls and produces dummy encoded packets.
class MockAudioEncoder : public AudioEncoder {
public:
    Result<void> Initialize(const AudioEncoderConfig& config) override {
        config_ = config;
        initialized_ = true;
        return {};
    }

    Result<void> Encode(const float* /*pcm*/, uint32_t frame_count,
                        Timestamp timestamp_us) override {
        encode_count_++;
        total_frames_ += frame_count;

        if (encode_error_) return Error::Make(*encode_error_, "mock encode");

        if (output_callback_) {
            EncodedAudioPacket packet;
            packet.data = {0xFC, 0x00};  // Dummy Opus TOC byte
            packet.timestamp_us = timestamp_us;
            packet.duration_us = 20000;  // 20ms
            output_callback_(std::move(packet));
        }

        return {};
    }

    Result<void> Flush() override { return {}; }

    void SetOutputCallback(EncodedAudioCallback callback) override {
        output_callback_ = std::move(callback);
    }

    Result<void> SetBitrate(uint32_t bitrate_bps) override {
        config_.bitrate_bps = bitrate_bps;
        return {};
    }

    AudioCodec GetCodec() const override { return config_.codec; }

    // Test helpers
    void FailEncodeWith(ErrorCode c) { encode_error_ = c; }
    bool IsInitialized() const { return initialized_; }
    uint32_t EncodeCount() const { return encode_count_; }
    uint32_t TotalFrames() const { return total_frames_; }

private:
    bool initialized_ = false;
    uint32_t encode_count_ = 0;
    uint32_t total_frames_ = 0;
    AudioEncoderConfig config_;
    EncodedAudioCallback output_callback_;
    std::optional<ErrorCode> encode_error_;
};

}  // namespace lumen
