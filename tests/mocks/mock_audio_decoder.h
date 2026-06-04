#pragma once

#include "lumen/codec/audio_decoder.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lumen {

/// Mock audio decoder for unit testing.
/// Produces synthetic PCM frames and supports error injection. DecodePLC()
/// is tracked separately so packet-loss-concealment paths can be asserted.
class MockAudioDecoder : public AudioDecoder {
public:
    Result<void> Initialize(const AudioDecoderConfig& config) override {
        if (init_error_) return Error::Make(*init_error_, "mock init");
        config_ = config;
        initialized_ = true;
        return {};
    }

    Result<DecodedAudioFrame> Decode(const uint8_t* /*data*/,
                                     size_t /*size*/) override {
        decode_count_++;
        if (decode_error_) return Error::Make(*decode_error_, "mock decode");
        return MakeFrame();
    }

    Result<DecodedAudioFrame> DecodePLC() override {
        plc_count_++;
        if (decode_error_) return Error::Make(*decode_error_, "mock plc");
        return MakeFrame();
    }

    AudioCodec GetCodec() const override { return config_.codec; }

    // ── Test controls ───────────────────────────────────────────────
    void FailInitializeWith(ErrorCode c) { init_error_ = c; }
    void FailDecodeWith(ErrorCode c) { decode_error_ = c; }
    void SetSamplesPerFrame(uint32_t n) { frames_per_call_ = n; }

    bool IsInitialized() const { return initialized_; }
    uint32_t DecodeCount() const { return decode_count_; }
    uint32_t PlcCount() const { return plc_count_; }

private:
    DecodedAudioFrame MakeFrame() const {
        DecodedAudioFrame f;
        f.channels = config_.channels;
        f.frame_count = frames_per_call_;
        f.samples.assign(
            static_cast<size_t>(frames_per_call_) * config_.channels, 0.0f);
        return f;
    }

    bool initialized_ = false;
    AudioDecoderConfig config_;
    uint32_t decode_count_ = 0;
    uint32_t plc_count_ = 0;
    uint32_t frames_per_call_ = 960;  // 20ms @ 48kHz
    std::optional<ErrorCode> init_error_;
    std::optional<ErrorCode> decode_error_;
};

}  // namespace lumen
