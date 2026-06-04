// Contract tests for the test doubles in tests/mocks/.
//
// Verifies each mock honors its interface on the happy path and surfaces
// injected errors. Pure interface + header-only mocks (no platform backends),
// so this runs in CI on any OS.

#include "mock_video_decoder.h"
#include "mock_audio_decoder.h"
#include "mock_color_converter.h"
#include "mock_video_renderer.h"
#include "mock_video_encoder.h"
#include "mock_audio_encoder.h"
#include "mock_transport_channel.h"

#include <cstdint>
#include <cstdio>

using namespace lumen;

namespace {
int passed = 0;
int failed = 0;
void check(bool cond, const char* msg) {
    if (cond) { ++passed; }
    else { std::fprintf(stderr, "FAIL: %s\n", msg); ++failed; }
}
}  // namespace

int main() {
    // ── MockVideoDecoder ────────────────────────────────────────────
    {
        MockVideoDecoder d;
        check(d.Initialize(VideoDecoderConfig{}, nullptr).ok(), "vdec init");
        auto r = d.Decode(nullptr, 10, 42);
        check(r.ok() && r.value().frame_index == 42, "vdec decode ok");
        d.ReleaseFrame(r.value());
        check(d.ReleaseCount() == 1, "vdec release counted");

        d.SetNeedMoreData(true);
        check(d.Decode(nullptr, 0, 43).error().code == ErrorCode::kTimeout,
              "vdec timeout");
        d.SetNeedMoreData(false);
        d.FailDecodeWith(ErrorCode::kDecoderError);
        check(d.Decode(nullptr, 0, 44).error().code == ErrorCode::kDecoderError,
              "vdec decode error");

        MockVideoDecoder d2;
        d2.FailInitializeWith(ErrorCode::kDeviceLost);
        check(d2.Initialize(VideoDecoderConfig{}, nullptr).error().code ==
                  ErrorCode::kDeviceLost,
              "vdec init error");
    }

    // ── MockAudioDecoder ────────────────────────────────────────────
    {
        MockAudioDecoder a;
        check(a.Initialize(AudioDecoderConfig{}).ok(), "adec init");
        auto r = a.Decode(nullptr, 5);
        check(r.ok() && r.value().frame_count == 960, "adec decode");
        check(a.DecodePLC().ok() && a.PlcCount() == 1, "adec plc");
        a.FailDecodeWith(ErrorCode::kAudioDecoderError);
        check(a.Decode(nullptr, 0).error().code ==
                  ErrorCode::kAudioDecoderError,
              "adec decode error");
    }

    // ── MockColorConverter ──────────────────────────────────────────
    {
        MockColorConverter c;
        VideoFrameDesc in;
        in.width = 320;
        in.height = 240;
        check(c.Initialize(in, PixelFormat::kNV12, nullptr).ok(), "cc init");
        check(c.GetOutputFormat() == PixelFormat::kNV12, "cc out format");
        check(c.Convert(nullptr, nullptr).ok() && c.ConvertCount() == 1,
              "cc convert");
        c.FailConvertWith(ErrorCode::kColorConversionError);
        check(c.Convert(nullptr, nullptr).error().code ==
                  ErrorCode::kColorConversionError,
              "cc convert error");
    }

    // ── MockVideoRenderer ───────────────────────────────────────────
    {
        MockVideoRenderer v;
        check(v.Initialize(320, 240, nullptr).ok(), "vr init");
        check(v.RenderFrame(nullptr, PixelFormat::kNV12).ok(), "vr render");
        check(v.Present().ok() && v.PresentCount() == 1, "vr present");
        check(!v.IsWindowClosed(), "vr open");
        v.SetWindowClosed(true);
        check(v.IsWindowClosed(), "vr closed");
        v.FailRenderWith(ErrorCode::kRendererError);
        check(v.RenderFrame(nullptr, PixelFormat::kNV12).error().code ==
                  ErrorCode::kRendererError,
              "vr render error");
    }

    // ── Existing mocks: error injection ─────────────────────────────
    {
        MockVideoEncoder e;
        e.Initialize(VideoEncoderConfig{}, nullptr);
        e.FailEncodeWith(ErrorCode::kEncoderError);
        check(e.Encode(nullptr, FrameMetadata{}).error().code ==
                  ErrorCode::kEncoderError,
              "venc encode error");

        MockAudioEncoder ae;
        ae.Initialize(AudioEncoderConfig{});
        ae.FailEncodeWith(ErrorCode::kAudioEncoderError);
        check(ae.Encode(nullptr, 0, 0).error().code ==
                  ErrorCode::kAudioEncoderError,
              "aenc encode error");

        MockTransportChannel ch(ChannelType::kVideo);
        uint8_t b[4] = {1, 2, 3, 4};
        check(ch.Send(b, 4, SendOptions{}).ok() && ch.SendCount() == 1,
              "chan send ok");
        ch.FailSendWith(ErrorCode::kTransportChannelFull);
        check(ch.Send(b, 4, SendOptions{}).error().code ==
                  ErrorCode::kTransportChannelFull,
              "chan send full");
        ch.SetSendCapacity(0);
        check(ch.GetSendCapacity() == 0, "chan backpressure");
    }

    std::printf("mock_contract_test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
