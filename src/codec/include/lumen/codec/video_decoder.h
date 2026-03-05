#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <cstddef>
#include <cstdint>

namespace lumen {

/// Abstract interface for hardware video decoders.
///
/// Implementations:
///   - MfVideoDecoder        (Windows, Media Foundation)
///   - VideoToolboxDecoder   (macOS / iOS)
///   - MediaCodecDecoder     (Android)
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    /// Initialize the decoder with the given configuration.
    /// `device` is the platform GPU device (e.g. ID3D11Device*) for zero-copy.
    virtual Result<void> Initialize(const VideoDecoderConfig& config,
                                    void* device) = 0;

    /// Decode a compressed bitstream chunk. Returns a decoded frame when one
    /// is available. Returns an error with kTimeout if the decoder needs more
    /// data before it can produce output.
    virtual Result<DecodedFrame> Decode(const uint8_t* data, size_t size,
                                        uint64_t frame_index) = 0;

    /// Flush any buffered frames (e.g. at end of stream).
    virtual Result<void> Flush() = 0;

    /// Release a decoded frame back to the decoder's texture pool.
    /// Must be called after the caller is done with the texture.
    virtual void ReleaseFrame(const DecodedFrame& frame) = 0;

    /// Return the codec this decoder accepts.
    virtual VideoCodec GetCodec() const = 0;
};

}  // namespace lumen
