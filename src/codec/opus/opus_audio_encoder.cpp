#include "opus_audio_encoder.h"

#include <cstring>

namespace lumen {

OpusAudioEncoder::OpusAudioEncoder() : encode_buffer_(4000) {}

OpusAudioEncoder::~OpusAudioEncoder() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
}

Result<void> OpusAudioEncoder::Initialize(const AudioEncoderConfig& config) {
    if (encoder_) {
        return Error::Make(ErrorCode::kAlreadyInitialized);
    }

    config_ = config;

    int error = 0;
    encoder_ = opus_encoder_create(config_.sample_rate, config_.channels,
                                    OPUS_APPLICATION_AUDIO, &error);
    if (error != OPUS_OK || !encoder_) {
        return Error::Make(ErrorCode::kAudioEncoderError,
                           std::string("opus_encoder_create failed: ") +
                               opus_strerror(error));
    }

    // Configure for high-quality screen sharing audio
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(config_.bitrate_bps));
    opus_encoder_ctl(encoder_, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

    accumulator_.reserve(kFrameSizeSamples * config_.channels * 2);
    return {};
}

Result<void> OpusAudioEncoder::Encode(const float* pcm, uint32_t frame_count,
                                       Timestamp timestamp_us) {
    if (!encoder_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    const uint32_t total_samples = frame_count * config_.channels;
    const float* src = pcm;
    uint32_t remaining = total_samples;

    // Track timestamp of first sample in accumulator
    if (!has_first_sample_ && accumulator_.empty()) {
        first_sample_time_us_ = timestamp_us;
        has_first_sample_ = true;
    }

    while (remaining > 0) {
        const uint32_t frame_samples = kFrameSizeSamples * config_.channels;
        const uint32_t space =
            frame_samples -
            static_cast<uint32_t>(accumulator_.size());
        const uint32_t to_copy = (remaining < space) ? remaining : space;

        accumulator_.insert(accumulator_.end(), src, src + to_copy);
        src += to_copy;
        remaining -= to_copy;

        if (accumulator_.size() == frame_samples) {
            EncodeAccumulatedFrames();
        }
    }

    return {};
}

void OpusAudioEncoder::EncodeAccumulatedFrames() {
    if (!output_callback_) {
        accumulator_.clear();
        has_first_sample_ = false;
        return;
    }

    int encoded_bytes = opus_encode_float(
        encoder_, accumulator_.data(), kFrameSizeSamples,
        encode_buffer_.data(),
        static_cast<opus_int32>(encode_buffer_.size()));

    if (encoded_bytes > 0) {
        EncodedAudioPacket packet;
        packet.data.assign(encode_buffer_.data(),
                           encode_buffer_.data() + encoded_bytes);
        packet.timestamp_us = first_sample_time_us_;
        // 20ms frame at 48kHz = 960 samples
        packet.duration_us = (kFrameSizeSamples * 1000000ULL) /
                             config_.sample_rate;
        output_callback_(std::move(packet));
    }

    accumulator_.clear();
    has_first_sample_ = false;
}

Result<void> OpusAudioEncoder::Flush() {
    if (!encoder_) return {};

    // Pad remaining samples with silence and encode
    if (!accumulator_.empty()) {
        const uint32_t frame_samples = kFrameSizeSamples * config_.channels;
        const uint32_t pad = frame_samples -
                             static_cast<uint32_t>(accumulator_.size());
        accumulator_.resize(frame_samples, 0.0f);
        EncodeAccumulatedFrames();
    }

    return {};
}

void OpusAudioEncoder::SetOutputCallback(EncodedAudioCallback callback) {
    output_callback_ = std::move(callback);
}

Result<void> OpusAudioEncoder::SetBitrate(uint32_t bitrate_bps) {
    if (!encoder_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    int ret = opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_bps));
    if (ret != OPUS_OK) {
        return Error::Make(ErrorCode::kAudioEncoderError,
                           std::string("OPUS_SET_BITRATE failed: ") +
                               opus_strerror(ret));
    }

    config_.bitrate_bps = bitrate_bps;
    return {};
}

AudioCodec OpusAudioEncoder::GetCodec() const { return AudioCodec::kOpus; }

}  // namespace lumen
