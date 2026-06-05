// Android backend — Phase 2.
//
// A0 scope: this translation unit only exercises the portable core + the POSIX
// UDP socket so they compile and link under the Android NDK (validating the
// Phase 1b POSIX implementation on a real Android toolchain). The actual
// Android backends — AMediaCodec encode/decode (A3), MediaProjection /
// AudioPlaybackCapture (A4), GLES/AAudio render (A5), JNI + Flutter (A6) —
// land in later subtasks behind the same abstract interfaces.

#include "lumen/jitter_buffer/video_jitter_buffer.h"
#include "lumen/transport/udp_socket.h"
#include "opus/opus_audio_decoder.h"
#include "codec/mediacodec_video_decoder.h"
#include "codec/mediacodec_video_encoder.h"

namespace lumen {

/// Link check: references the POSIX socket, jitter buffer, Opus decoder, and
/// the AMediaCodec video encoder/decoder so the Android build fails fast on any
/// portability or link regression (the MediaCodec refs force libmediandk to
/// resolve). Not run here — building android_quiche_linkcheck is the check.
bool AndroidCoreLinkCheck() {
    std::unique_ptr<UdpSocket> sock = MakeUdpSocket();
    VideoJitterBuffer jb;
    OpusAudioDecoder dec;
    const bool dec_ok = dec.Initialize(AudioDecoderConfig{}).ok();

    MediaCodecVideoEncoder venc;
    MediaCodecVideoDecoder vdec;
    const bool codec_linked = venc.GetCodec() == VideoCodec::kH264 &&
                              vdec.GetCodec() == VideoCodec::kH264;

    return sock != nullptr && jb.Size() == 0 && dec_ok && codec_linked;
}

}  // namespace lumen
