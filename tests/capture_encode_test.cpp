/// End-to-end test for the Windows zero-copy capture -> encode pipeline.
///
/// Captures frames from the primary monitor via DXGI Desktop Duplication,
/// converts BGRA->NV12 via D3D11 Video Processor, and encodes to H.264
/// via the Media Foundation hardware encoder.
///
/// Reports per-frame and aggregate latency statistics.
/// Writes raw H.264 bitstream to output.h264 for verification.

#include "common/d3d11_device_context.h"
#include "capture/dxgi_desktop_duplication.h"
#include "codec/d3d11_color_converter.h"
#include "codec/mf_video_encoder.h"

#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <vector>

using namespace lumen;
using Clock = std::chrono::high_resolution_clock;

struct FrameTiming {
    double capture_ms;
    double convert_ms;    // BGRA -> NV12
    double encode_ms;     // MFT ProcessInput + ProcessOutput
    double total_ms;      // End-to-end for this frame
};

static void PrintStats(const std::vector<FrameTiming>& timings) {
    if (timings.empty()) {
        printf("No frames captured.\n");
        return;
    }

    auto avg = [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    };

    auto percentile = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        size_t idx = static_cast<size_t>(p / 100.0 * (v.size() - 1));
        return v[idx];
    };

    std::vector<double> captures, converts, encodes, totals;
    for (auto& t : timings) {
        captures.push_back(t.capture_ms);
        converts.push_back(t.convert_ms);
        encodes.push_back(t.encode_ms);
        totals.push_back(t.total_ms);
    }

    printf("\n=== Pipeline Performance (%zu frames) ===\n", timings.size());
    printf("%-20s %8s %8s %8s %8s\n", "Stage", "Avg", "P50", "P95", "P99");
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Capture+Copy",
           avg(captures), percentile(captures, 50),
           percentile(captures, 95), percentile(captures, 99));
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Color Convert",
           avg(converts), percentile(converts, 50),
           percentile(converts, 95), percentile(converts, 99));
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Encode",
           avg(encodes), percentile(encodes, 50),
           percentile(encodes, 95), percentile(encodes, 99));
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Total",
           avg(totals), percentile(totals, 50),
           percentile(totals, 95), percentile(totals, 99));

    double total_time_s = 0;
    for (auto& t : totals) total_time_s += t;
    total_time_s /= 1000.0;
    printf("\nEffective FPS: %.1f\n", timings.size() / total_time_s);
}

int main() {
    printf("Lumen Zero-Copy Pipeline Test\n");
    printf("=============================\n\n");

    // Initialize COM and Media Foundation.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    constexpr uint32_t kOutputIndex = 0;
    constexpr uint32_t kNumFrames = 300;  // ~5 seconds at 60fps
    constexpr uint32_t kTimeoutMs = 100;

    // 1. Create shared D3D11 device on the primary display adapter.
    printf("[1/5] Creating D3D11 device...\n");
    D3D11DeviceContext device_ctx;
    auto result = device_ctx.Initialize(kOutputIndex);
    if (!result) {
        printf("FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  OK\n");

    // 2. Initialize DXGI Desktop Duplication capture.
    printf("[2/5] Initializing DXGI Desktop Duplication...\n");
    DxgiDesktopDuplication capture(device_ctx);
    result = capture.Initialize(kOutputIndex);
    if (!result) {
        printf("FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    auto desc = capture.GetFrameDesc();
    printf("  OK - %ux%u BGRA\n", desc.width, desc.height);

    // 3. Initialize color converter (BGRA -> NV12).
    printf("[3/5] Initializing D3D11 Video Processor (BGRA->NV12)...\n");
    D3D11ColorConverter converter(device_ctx);
    result = converter.Initialize(desc, PixelFormat::kNV12, nullptr);
    if (!result) {
        printf("FAILED: %s\n", result.error().message.c_str());
        return 1;
    }

    // Create the NV12 output texture.
    auto nv12_result = device_ctx.CreateNV12Texture(desc.width, desc.height);
    if (!nv12_result) {
        printf("FAILED to create NV12 texture: %s\n",
               nv12_result.error().message.c_str());
        return 1;
    }
    auto nv12_texture = std::move(nv12_result).value();
    printf("  OK\n");

    // 4. Initialize Media Foundation hardware encoder.
    printf("[4/5] Initializing MF hardware encoder (H.264)...\n");
    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig enc_config;
    enc_config.width = desc.width;
    enc_config.height = desc.height;
    enc_config.fps = 60;
    enc_config.bitrate_bps = 8'000'000;
    enc_config.codec = VideoCodec::kH264;
    enc_config.low_latency = true;

    uint64_t total_encoded_bytes = 0;
    uint32_t keyframe_count = 0;
    uint32_t packet_count = 0;

    // Open file for bitstream output.
    std::ofstream bitstream_file("output.h264", std::ios::binary);
    if (!bitstream_file) {
        printf("WARNING: Could not open output.h264 for writing\n");
    }

    encoder.SetOutputCallback([&](EncodedPacket packet) {
        total_encoded_bytes += packet.data.size();
        packet_count++;
        if (packet.metadata.is_keyframe) keyframe_count++;
        if (bitstream_file) {
            bitstream_file.write(
                reinterpret_cast<const char*>(packet.data.data()),
                packet.data.size());
        }
    });

    result = encoder.Initialize(enc_config, nullptr);
    if (!result) {
        printf("FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  OK - H.264 %u kbps, %u fps, low-latency\n",
           enc_config.bitrate_bps / 1000, enc_config.fps);

    // 5. Run the capture -> convert -> encode loop.
    printf("[5/5] Capturing and encoding %u frames...\n\n", kNumFrames);

    std::vector<FrameTiming> timings;
    timings.reserve(kNumFrames);
    uint32_t timeout_count = 0;
    uint32_t error_count = 0;
    uint32_t max_errors = 10;  // Stop after this many consecutive errors

    uint32_t consecutive_errors = 0;

    for (uint32_t i = 0; timings.size() < kNumFrames; ++i) {
        if (timeout_count > kNumFrames * 3) {
            printf("Too many timeouts, aborting.\n");
            break;
        }
        if (consecutive_errors >= max_errors) {
            printf("Too many consecutive errors (%u), aborting.\n", max_errors);
            break;
        }

        auto frame_start = Clock::now();

        // Capture
        auto capture_result = capture.AcquireFrame(kTimeoutMs);
        auto after_capture = Clock::now();

        if (!capture_result) {
            if (capture_result.error().code == ErrorCode::kTimeout) {
                timeout_count++;
                continue;
            }
            if (capture_result.error().code == ErrorCode::kAccessLost) {
                timeout_count++;
                continue;
            }
            printf("Capture error: %s\n", capture_result.error().message.c_str());
            error_count++;
            consecutive_errors++;
            continue;
        }

        auto& frame = capture_result.value();

        // Color convert (BGRA -> NV12)
        auto convert_start = Clock::now();
        result = converter.Convert(frame.native_texture, nv12_texture.Get());
        auto after_convert = Clock::now();

        if (!result) {
            printf("Convert error: %s\n", result.error().message.c_str());
            capture.ReleaseFrame();
            error_count++;
            consecutive_errors++;
            continue;
        }

        // Encode
        auto encode_start = Clock::now();
        result = encoder.Encode(nv12_texture.Get(), frame.metadata);
        auto after_encode = Clock::now();

        capture.ReleaseFrame();

        if (!result) {
            printf("Encode error: %s\n", result.error().message.c_str());
            error_count++;
            consecutive_errors++;
            continue;
        }

        consecutive_errors = 0;

        // Record timings.
        FrameTiming t;
        t.capture_ms = std::chrono::duration<double, std::milli>(
            after_capture - frame_start).count();
        t.convert_ms = std::chrono::duration<double, std::milli>(
            after_convert - convert_start).count();
        t.encode_ms = std::chrono::duration<double, std::milli>(
            after_encode - encode_start).count();
        t.total_ms = std::chrono::duration<double, std::milli>(
            after_encode - frame_start).count();

        timings.push_back(t);

        // Progress indicator every 60 frames.
        if (timings.size() % 60 == 0) {
            printf("  %zu/%u frames (%.1f ms last, %u packets)\n",
                   timings.size(), kNumFrames, t.total_ms, packet_count);
        }
    }

    // Flush encoder.
    encoder.Flush();
    bitstream_file.close();

    // Print results.
    PrintStats(timings);

    printf("\nEncoded: %u packets, %.2f MB (%u keyframes)\n",
           packet_count, total_encoded_bytes / (1024.0 * 1024.0), keyframe_count);
    printf("Timeouts: %u, Errors: %u\n", timeout_count, error_count);

    if (total_encoded_bytes > 0) {
        printf("\nBitstream written to output.h264\n");
        printf("Verify with: ffprobe -v error -show_entries stream=codec_name,"
               "width,height,r_frame_rate -of default=noprint_wrappers=1 output.h264\n");
        printf("Or play with: ffplay output.h264\n");
    }

    // Cleanup.
    MFShutdown();
    CoUninitialize();

    return (timings.empty() || packet_count == 0) ? 1 : 0;
}
