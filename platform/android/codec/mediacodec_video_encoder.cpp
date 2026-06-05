#include "mediacodec_video_encoder.h"

#include <media/NdkMediaFormat.h>

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
// MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface.
constexpr int32_t kColorFormatSurface = 0x7F000789;
constexpr int64_t kDequeueTimeoutUs = 0;  // non-blocking poll
}  // namespace

MediaCodecVideoEncoder::~MediaCodecVideoEncoder() { Teardown(); }

void MediaCodecVideoEncoder::Teardown() {
    if (codec_) {
        if (started_) AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
    if (input_surface_) {
        ANativeWindow_release(input_surface_);
        input_surface_ = nullptr;
    }
    started_ = false;
}

Result<void> MediaCodecVideoEncoder::Initialize(const VideoEncoderConfig& config,
                                                void* /*device*/) {
    config_ = config;

    codec_ = AMediaCodec_createEncoderByType(MimeFor(config.codec));
    if (!codec_) {
        return Error::Make(ErrorCode::kEncoderError,
                           "AMediaCodec_createEncoderByType failed");
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, MimeFor(config.codec));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,
                          static_cast<int32_t>(config.width));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT,
                          static_cast<int32_t>(config.height));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                          kColorFormatSurface);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE,
                          static_cast<int32_t>(config.bitrate_bps));
    AMediaFormat_setFloat(fmt, AMEDIAFORMAT_KEY_FRAME_RATE,
                          static_cast<float>(config.fps));
    // Keyframe interval in seconds (0 ⇒ derive from gop, else 1s default).
    const int32_t iframe_s =
        config.gop_size_frames && config.fps
            ? static_cast<int32_t>(config.gop_size_frames / config.fps)
            : 1;
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, iframe_s);

    const media_status_t st = AMediaCodec_configure(
        codec_, fmt, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
        Teardown();
        return Error::Make(ErrorCode::kEncoderError, "AMediaCodec_configure failed");
    }

    if (AMediaCodec_createInputSurface(codec_, &input_surface_) != AMEDIA_OK) {
        Teardown();
        return Error::Make(ErrorCode::kEncoderError,
                           "AMediaCodec_createInputSurface failed");
    }

    if (AMediaCodec_start(codec_) != AMEDIA_OK) {
        Teardown();
        return Error::Make(ErrorCode::kEncoderError, "AMediaCodec_start failed");
    }
    started_ = true;
    return {};
}

void MediaCodecVideoEncoder::DrainOutput() {
    while (true) {
        AMediaCodecBufferInfo info;
        const ssize_t idx =
            AMediaCodec_dequeueOutputBuffer(codec_, &info, kDequeueTimeoutUs);
        if (idx < 0) break;  // no more output ready

        size_t cap = 0;
        uint8_t* out =
            AMediaCodec_getOutputBuffer(codec_, static_cast<size_t>(idx), &cap);
        if (out && info.size > 0 && output_callback_) {
            EncodedPacket pkt;
            pkt.data.assign(out + info.offset, out + info.offset + info.size);
            pkt.metadata.is_keyframe =
                (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
            pkt.metadata.capture_time_us = info.presentationTimeUs;
            output_callback_(std::move(pkt));
        }
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(idx),
                                        /*render=*/false);
    }
}

Result<void> MediaCodecVideoEncoder::Encode(void* /*native_texture*/,
                                            const FrameMetadata& /*metadata*/) {
    if (!codec_) return Error::Make(ErrorCode::kEncoderError, "not initialized");

    // Surface-input encoder: pixels arrive via input_surface_ (drawn by the
    // capture/GL stage, A4). Apply a pending keyframe request, then drain.
    if (keyframe_requested_.exchange(false)) {
        AMediaFormat* params = AMediaFormat_new();
        AMediaFormat_setInt32(params, "request-sync", 0);
        AMediaCodec_setParameters(codec_, params);
        AMediaFormat_delete(params);
    }
    DrainOutput();
    return {};
}

Result<void> MediaCodecVideoEncoder::Flush() {
    if (codec_ && AMediaCodec_flush(codec_) != AMEDIA_OK) {
        return Error::Make(ErrorCode::kEncoderError, "AMediaCodec_flush failed");
    }
    return {};
}

Result<void> MediaCodecVideoEncoder::SetBitrate(uint32_t bitrate_bps) {
    config_.bitrate_bps = bitrate_bps;
    if (!codec_) return {};
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, "video-bitrate",
                          static_cast<int32_t>(bitrate_bps));
    const media_status_t st = AMediaCodec_setParameters(codec_, params);
    AMediaFormat_delete(params);
    if (st != AMEDIA_OK) {
        return Error::Make(ErrorCode::kEncoderError, "set bitrate failed");
    }
    return {};
}

Result<void> MediaCodecVideoEncoder::SetFrameRate(uint32_t fps) {
    // MediaCodec has no runtime frame-rate knob; record it (a Reconfigure
    // applies it). Pacing is handled upstream by FramePacer.
    config_.fps = fps;
    return {};
}

Result<void> MediaCodecVideoEncoder::Reconfigure(
    const VideoEncoderConfig& new_config) {
    Teardown();
    return Initialize(new_config, nullptr);
}

}  // namespace lumen
