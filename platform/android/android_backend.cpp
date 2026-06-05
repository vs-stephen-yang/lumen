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

namespace lumen {

/// Link check: pulls in MakeUdpSocket() (POSIX impl), the jitter buffer, and
/// the Opus decoder (libopus, arm64-android) so the Android core build fails
/// fast on any portability or link regression.
bool AndroidCoreLinkCheck() {
    std::unique_ptr<UdpSocket> sock = MakeUdpSocket();
    VideoJitterBuffer jb;
    OpusAudioDecoder dec;
    const bool dec_ok = dec.Initialize(AudioDecoderConfig{}).ok();
    return sock != nullptr && jb.Size() == 0 && dec_ok;
}

}  // namespace lumen
