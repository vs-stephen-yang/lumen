#pragma once

#include "lumen/codec/video_encoder.h"

#include <media/NdkMediaCodec.h>
#include <android/native_window.h>

#include <atomic>

namespace lumen {

/// Android hardware video encoder on NDK AMediaCodec.
///
/// Zero-copy capture path: the encoder owns an input Surface (ANativeWindow)
/// that the capture/GL stage draws frames into (wired in A4); GetInputSurface()
/// exposes it. Encoded bitstream is drained to EncodedPacket and delivered via
/// the output callback. Because input arrives through the Surface, Encode()
/// does not carry pixels — it pumps the output drain for the just-produced
/// frame and applies any pending keyframe request.
class MediaCodecVideoEncoder : public VideoEncoder {
public:
    MediaCodecVideoEncoder() = default;
    ~MediaCodecVideoEncoder() override;

    MediaCodecVideoEncoder(const MediaCodecVideoEncoder&) = delete;
    MediaCodecVideoEncoder& operator=(const MediaCodecVideoEncoder&) = delete;

    Result<void> Initialize(const VideoEncoderConfig& config,
                            void* device) override;
    Result<void> Encode(void* native_texture,
                        const FrameMetadata& metadata) override;
    Result<void> Flush() override;
    void SetOutputCallback(EncodedPacketCallback callback) override {
        output_callback_ = std::move(callback);
    }
    void RequestKeyframe() override { keyframe_requested_.store(true); }
    Result<void> SetBitrate(uint32_t bitrate_bps) override;
    Result<void> SetFrameRate(uint32_t fps) override;
    Result<void> Reconfigure(const VideoEncoderConfig& new_config) override;
    VideoCodec GetCodec() const override { return config_.codec; }

    /// The Surface the capture/GL stage draws into (valid after Initialize()).
    ANativeWindow* GetInputSurface() const { return input_surface_; }

private:
    void Teardown();
    void DrainOutput();

    VideoEncoderConfig config_;
    AMediaCodec* codec_ = nullptr;
    ANativeWindow* input_surface_ = nullptr;  // owned (created by the codec)
    EncodedPacketCallback output_callback_;
    std::atomic<bool> keyframe_requested_{false};
    bool started_ = false;
};

}  // namespace lumen
