#include "mediacodec_video_decoder.h"

#include <media/NdkMediaFormat.h>

#include <cstring>

namespace lumen {

namespace {
const char* MimeFor(VideoCodec c) {
    switch (c) {
        case VideoCodec::kH264: return "video/avc";
        case VideoCodec::kHEVC: return "video/hevc";
        case VideoCodec::kAV1:  return "video/av01";
    }
    return "video/avc";
}
constexpr int64_t kDequeueTimeoutUs = 0;  // non-blocking poll
}  // namespace

MediaCodecVideoDecoder::~MediaCodecVideoDecoder() { Teardown(); }

void MediaCodecVideoDecoder::Teardown() {
    if (codec_) {
        if (started_) AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    started_ = false;
}

Result<void> MediaCodecVideoDecoder::Initialize(const VideoDecoderConfig& config,
                                                void* device) {
    config_ = config;
    output_surface_ = static_cast<ANativeWindow*>(device);  // may be null

    codec_ = AMediaCodec_createDecoderByType(MimeFor(config.codec));
    if (!codec_) {
        return Error::Make(ErrorCode::kDecoderError,
                           "AMediaCodec_createDecoderByType failed");
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, MimeFor(config.codec));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,
                          static_cast<int32_t>(config.width));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT,
                          static_cast<int32_t>(config.height));
    if (config.low_latency) {
        // Best-effort low-latency hint (honored on API 30+; ignored otherwise).
        AMediaFormat_setInt32(fmt, "low-latency", 1);
    }

    const media_status_t st =
        AMediaCodec_configure(codec_, fmt, output_surface_, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        Teardown();
        return Error::Make(ErrorCode::kDecoderError, "AMediaCodec_configure failed");
    }

    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        Teardown();
        return Error::Make(ErrorCode::kDecoderError, "AMediaCodec_start failed");
    }
    started_ = true;
    return {};
}

Result<DecodedFrame> MediaCodecVideoDecoder::Decode(const uint8_t* data,
                                                    size_t size,
                                                    uint64_t frame_index) {
    if (!codec_) return Error::Make(ErrorCode::kDecoderError, "not initialized");

    // Submit the access unit.
    const ssize_t in_idx = AMediaCodec_dequeueInputBuffer(codec_, kDequeueTimeoutUs);
    if (in_idx >= 0) {
        size_t cap = 0;
        uint8_t* in = AMediaCodec_getInputBuffer(codec_,
                                                 static_cast<size_t>(in_idx),
                                                 &cap);
        if (in && size <= cap) {
            std::memcpy(in, data, size);
            AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(in_idx),
                                         /*offset=*/0, size,
                                         /*time=*/frame_index, /*flags=*/0);
        }
    }

    // Drain one decoded frame if ready.
    AMediaCodecBufferInfo info;
    const ssize_t out_idx =
        AMediaCodec_dequeueOutputBuffer(codec_, &info, kDequeueTimeoutUs);
    if (out_idx >= 0) {
        // render == true presents directly onto the output Surface (zero-copy);
        // with no surface this just recycles the buffer.
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(out_idx),
                                        /*render=*/output_surface_ != nullptr);
        DecodedFrame f;
        f.native_texture = nullptr;  // surface-backed; sampled by the renderer (A5)
        f.width = config_.width;
        f.height = config_.height;
        f.format = PixelFormat::kNV12;
        f.frame_index = frame_index;
        return f;
    }

    // No output yet (codec needs more input / still buffering).
    return Error::Make(ErrorCode::kTimeout);
}

Result<void> MediaCodecVideoDecoder::Flush() {
    if (codec_ && AMediaCodec_flush(codec_) != AMEDIA_OK) {
        return Error::Make(ErrorCode::kDecoderError, "AMediaCodec_flush failed");
    }
    return {};
}

void MediaCodecVideoDecoder::ReleaseFrame(const DecodedFrame& /*frame*/) {
    // Surface-backed frames are released at dequeue time (render=true); nothing
    // to recycle here. Buffer-mode CPU output would be freed here in future.
}

}  // namespace lumen
