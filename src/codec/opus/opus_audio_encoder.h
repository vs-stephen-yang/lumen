#pragma once

#include "lumen/codec/audio_encoder.h"

#include <opus.h>

#include <vector>

namespace lumen {

/// Opus audio encoder using libopus.
///
/// Cross-platform — same implementation on Windows, macOS, iOS, Android.
/// Accumulates PCM samples and encodes complete 20ms Opus frames (960 samples
/// at 48kHz). Uses OPUS_APPLICATION_AUDIO for high-fidelity screen sharing
/// audio.
class OpusAudioEncoder : public AudioEncoder {
public:
    OpusAudioEncoder();
    ~OpusAudioEncoder() override;

    OpusAudioEncoder(const OpusAudioEncoder&) = delete;
    OpusAudioEncoder& operator=(const OpusAudioEncoder&) = delete;

    Result<void> Initialize(const AudioEncoderConfig& config) override;
    Result<void> Encode(const float* pcm, uint32_t frame_count,
                        Timestamp timestamp_us) override;
    Result<void> Flush() override;
    void SetOutputCallback(EncodedAudioCallback callback) override;
    Result<void> SetBitrate(uint32_t bitrate_bps) override;
    AudioCodec GetCodec() const override;

private:
    void EncodeAccumulatedFrames();

    OpusEncoder* encoder_ = nullptr;
    AudioEncoderConfig config_;
    EncodedAudioCallback output_callback_;

    // Frame accumulator: collect exactly 960 samples (20ms at 48kHz)
    static constexpr uint32_t kFrameSizeSamples = 960;
    std::vector<float> accumulator_;
    Timestamp first_sample_time_us_ = 0;
    bool has_first_sample_ = false;

    // Encode output buffer (4000 bytes is safe max for any Opus packet)
    std::vector<uint8_t> encode_buffer_;
};

}  // namespace lumen
