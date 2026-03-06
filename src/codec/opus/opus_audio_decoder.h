#pragma once

#include "lumen/codec/audio_decoder.h"

#include <opus.h>

#include <vector>

namespace lumen {

/// Opus audio decoder using libopus.
///
/// Cross-platform — same implementation on Windows, macOS, iOS, Android.
/// Supports packet loss concealment (PLC) for graceful degradation when
/// packets are lost in transit.
class OpusAudioDecoder : public AudioDecoder {
public:
    OpusAudioDecoder();
    ~OpusAudioDecoder() override;

    OpusAudioDecoder(const OpusAudioDecoder&) = delete;
    OpusAudioDecoder& operator=(const OpusAudioDecoder&) = delete;

    Result<void> Initialize(const AudioDecoderConfig& config) override;
    Result<DecodedAudioFrame> Decode(const uint8_t* data,
                                      size_t size) override;
    Result<DecodedAudioFrame> DecodePLC() override;
    AudioCodec GetCodec() const override;

private:
    OpusDecoder* decoder_ = nullptr;
    AudioDecoderConfig config_;

    // Decode buffer: 5760 samples per channel max (120ms at 48kHz)
    static constexpr uint32_t kMaxFrameSizeSamples = 5760;
    std::vector<float> decode_buffer_;
};

}  // namespace lumen
