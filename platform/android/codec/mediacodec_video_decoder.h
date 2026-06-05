#pragma once

#include "lumen/codec/video_decoder.h"

#include <media/NdkMediaCodec.h>
#include <android/native_window.h>

namespace lumen {

/// Android hardware video decoder on NDK AMediaCodec.
///
/// Zero-copy receive path: the decoder is configured with an output Surface
/// (ANativeWindow) and renders decoded frames straight onto it for GL/Vulkan
/// presentation (the renderer owns that Surface — wired in A5). The output
/// Surface is passed through VideoDecoder::Initialize()'s `device` parameter
/// as an `ANativeWindow*`. When `device` is null the decoder runs in buffer
/// mode (decoded frames are released without rendering) — useful for headless
/// bring-up before the renderer exists.
class MediaCodecVideoDecoder : public VideoDecoder {
public:
    MediaCodecVideoDecoder() = default;
    ~MediaCodecVideoDecoder() override;

    MediaCodecVideoDecoder(const MediaCodecVideoDecoder&) = delete;
    MediaCodecVideoDecoder& operator=(const MediaCodecVideoDecoder&) = delete;

    Result<void> Initialize(const VideoDecoderConfig& config,
                            void* device) override;
    Result<DecodedFrame> Decode(const uint8_t* data, size_t size,
                                uint64_t frame_index) override;
    Result<void> Flush() override;
    void ReleaseFrame(const DecodedFrame& frame) override;
    VideoCodec GetCodec() const override { return config_.codec; }

private:
    void Teardown();

    VideoDecoderConfig config_;
    AMediaCodec* codec_ = nullptr;
    ANativeWindow* output_surface_ = nullptr;  // non-owning (renderer owns it)
    bool started_ = false;
};

}  // namespace lumen
