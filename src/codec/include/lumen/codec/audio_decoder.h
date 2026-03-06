#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lumen {

/// A decoded audio frame (PCM samples on CPU).
struct DecodedAudioFrame {
    std::vector<float> samples;    // Interleaved float32 PCM
    uint32_t frame_count = 0;      // Number of samples per channel
    uint32_t channels = 2;
    Timestamp timestamp_us = 0;
};

/// Abstract interface for audio decoders.
///
/// Implementations:
///   - OpusAudioDecoder  (cross-platform, libopus)
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;

    /// Initialize the decoder with the given configuration.
    virtual Result<void> Initialize(const AudioDecoderConfig& config) = 0;

    /// Decode an encoded audio packet.
    /// Returns a decoded audio frame with PCM samples.
    virtual Result<DecodedAudioFrame> Decode(const uint8_t* data,
                                              size_t size) = 0;

    /// Perform packet loss concealment (PLC).
    /// Generates replacement audio when a packet is lost.
    virtual Result<DecodedAudioFrame> DecodePLC() = 0;

    /// Return the codec this decoder accepts.
    virtual AudioCodec GetCodec() const = 0;
};

}  // namespace lumen
