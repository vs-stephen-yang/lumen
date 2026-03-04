#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <functional>
#include <memory>
#include <vector>

namespace lumen {

/// Callback invoked when an encoded packet is ready.
using EncodedPacketCallback = std::function<void(EncodedPacket)>;

/// Abstract interface for hardware video encoders.
///
/// Implementations:
///   - MfVideoEncoder       (Windows, Media Foundation — works with any GPU vendor)
///   - NvencVideoEncoder    (Windows, NVIDIA NVENC direct SDK)
///   - AmfVideoEncoder      (Windows, AMD AMF)
///   - VideoToolboxEncoder  (macOS / iOS)
///   - MediaCodecEncoder    (Android)
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    /// Initialize the encoder with the given configuration.
    /// `device` is the platform GPU device (e.g. ID3D11Device*) that must be
    /// shared with the capture backend for zero-copy operation.
    virtual Result<void> Initialize(const VideoEncoderConfig& config,
                                    void* device) = 0;

    /// Encode a single frame. The texture must reside on the same GPU device
    /// that was passed to Initialize().
    ///
    /// `native_texture` — platform GPU texture (e.g. ID3D11Texture2D*)
    /// `metadata`       — frame timing and index information
    ///
    /// Encoded output is delivered asynchronously via the callback registered
    /// with SetOutputCallback().
    virtual Result<void> Encode(void* native_texture,
                                const FrameMetadata& metadata) = 0;

    /// Flush any buffered frames (e.g. at end of stream).
    virtual Result<void> Flush() = 0;

    /// Register the callback that receives encoded packets.
    virtual void SetOutputCallback(EncodedPacketCallback callback) = 0;

    /// Request an IDR (keyframe) on the next Encode() call.
    virtual void RequestKeyframe() = 0;

    /// Dynamically change bitrate (in bits per second).
    virtual Result<void> SetBitrate(uint32_t bitrate_bps) = 0;

    /// Return the codec this encoder produces.
    virtual VideoCodec GetCodec() const = 0;
};

}  // namespace lumen
