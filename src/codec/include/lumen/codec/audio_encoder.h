#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <functional>

namespace lumen {

/// Callback invoked when an encoded audio packet is ready.
using EncodedAudioCallback = std::function<void(EncodedAudioPacket)>;

/// Abstract interface for audio encoders.
///
/// Implementations:
///   - OpusAudioEncoder  (cross-platform, libopus)
class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;

    /// Initialize the encoder with the given configuration.
    virtual Result<void> Initialize(const AudioEncoderConfig& config) = 0;

    /// Encode PCM audio samples.
    /// `pcm` — interleaved float32 samples
    /// `frame_count` — number of samples per channel
    /// `timestamp_us` — capture timestamp of the first sample
    ///
    /// Encoded output is delivered via the callback registered with
    /// SetOutputCallback(). The encoder accumulates samples internally
    /// and encodes complete Opus frames (typically 20ms / 960 samples).
    virtual Result<void> Encode(const float* pcm, uint32_t frame_count,
                                Timestamp timestamp_us) = 0;

    /// Flush any buffered samples (e.g. at end of stream).
    virtual Result<void> Flush() = 0;

    /// Register the callback that receives encoded packets.
    virtual void SetOutputCallback(EncodedAudioCallback callback) = 0;

    /// Dynamically change bitrate (in bits per second).
    virtual Result<void> SetBitrate(uint32_t bitrate_bps) = 0;

    /// Return the codec this encoder produces.
    virtual AudioCodec GetCodec() const = 0;
};

}  // namespace lumen
