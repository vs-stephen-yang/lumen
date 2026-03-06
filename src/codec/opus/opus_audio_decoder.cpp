#include "opus_audio_decoder.h"

namespace lumen {

OpusAudioDecoder::OpusAudioDecoder() = default;

OpusAudioDecoder::~OpusAudioDecoder() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
}

Result<void> OpusAudioDecoder::Initialize(const AudioDecoderConfig& config) {
    if (decoder_) {
        return Error::Make(ErrorCode::kAlreadyInitialized);
    }

    config_ = config;

    int error = 0;
    decoder_ = opus_decoder_create(config_.sample_rate, config_.channels,
                                    &error);
    if (error != OPUS_OK || !decoder_) {
        return Error::Make(ErrorCode::kAudioDecoderError,
                           std::string("opus_decoder_create failed: ") +
                               opus_strerror(error));
    }

    decode_buffer_.resize(kMaxFrameSizeSamples * config_.channels);
    return {};
}

Result<DecodedAudioFrame> OpusAudioDecoder::Decode(const uint8_t* data,
                                                     size_t size) {
    if (!decoder_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    int decoded_samples = opus_decode_float(
        decoder_, data, static_cast<opus_int32>(size),
        decode_buffer_.data(), kMaxFrameSizeSamples, 0);

    if (decoded_samples < 0) {
        return Error::Make(ErrorCode::kAudioDecoderError,
                           std::string("opus_decode_float failed: ") +
                               opus_strerror(decoded_samples));
    }

    DecodedAudioFrame frame;
    frame.frame_count = static_cast<uint32_t>(decoded_samples);
    frame.channels = config_.channels;
    frame.samples.assign(
        decode_buffer_.data(),
        decode_buffer_.data() + decoded_samples * config_.channels);

    return frame;
}

Result<DecodedAudioFrame> OpusAudioDecoder::DecodePLC() {
    if (!decoder_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    // PLC: pass nullptr data to generate concealment audio
    // Use 960 samples (20ms) as the expected frame size
    constexpr int kExpectedFrameSize = 960;
    int decoded_samples = opus_decode_float(
        decoder_, nullptr, 0, decode_buffer_.data(), kExpectedFrameSize, 0);

    if (decoded_samples < 0) {
        return Error::Make(ErrorCode::kAudioDecoderError,
                           std::string("opus_decode_float PLC failed: ") +
                               opus_strerror(decoded_samples));
    }

    DecodedAudioFrame frame;
    frame.frame_count = static_cast<uint32_t>(decoded_samples);
    frame.channels = config_.channels;
    frame.samples.assign(
        decode_buffer_.data(),
        decode_buffer_.data() + decoded_samples * config_.channels);

    return frame;
}

AudioCodec OpusAudioDecoder::GetCodec() const { return AudioCodec::kOpus; }

}  // namespace lumen
