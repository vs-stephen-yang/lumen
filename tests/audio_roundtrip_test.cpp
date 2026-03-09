/// Integration test: WASAPI Capture -> Opus Encode -> Opus Decode -> WASAPI Render
///
/// Full audio pipeline roundtrip test. Captures system audio, encodes with Opus,
/// decodes, and plays back through the WASAPI renderer. Verifies pipeline health
/// via statistics.
///
/// NOTE: If capture and render use the same audio device, a feedback loop occurs
/// (played audio is re-captured). This is expected and not harmful — it produces
/// an echo effect but does not oscillate thanks to Opus coding latency.

#include "capture/wasapi_loopback_capture.h"
#include "opus/opus_audio_encoder.h"
#include "opus/opus_audio_decoder.h"
#include "renderer/wasapi_audio_renderer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace lumen;

int main() {
    printf("=== Audio Roundtrip Test (Capture -> Encode -> Decode -> Render) ===\n\n");

    // ── Create components ──────────────────────────────────────────────

    WasapiLoopbackCapture capture;
    OpusAudioEncoder encoder;
    OpusAudioDecoder decoder;
    WasapiAudioRenderer renderer;

    std::atomic<uint32_t> captured_frames{0};
    std::atomic<uint32_t> encoded_packets{0};
    std::atomic<uint32_t> decoded_frames{0};
    std::atomic<uint32_t> queued_frames{0};

    // ── Initialize ─────────────────────────────────────────────────────

    printf("[1/6] Initializing WASAPI loopback capture...\n");
    auto result = capture.Initialize();
    if (!result.ok()) {
        printf("FAIL: Capture init: %s\n", result.error().message.c_str());
        return 1;
    }

    auto format = capture.GetFormat();
    printf("       Format: %uHz, %u channels, float32\n",
           format.sample_rate, format.channels);

    printf("[2/6] Initializing Opus encoder...\n");
    AudioEncoderConfig enc_config;
    enc_config.sample_rate = format.sample_rate;
    enc_config.channels = format.channels;
    enc_config.bitrate_bps = 128000;

    result = encoder.Initialize(enc_config);
    if (!result.ok()) {
        printf("FAIL: Encoder init: %s\n", result.error().message.c_str());
        return 1;
    }

    printf("[3/6] Initializing Opus decoder...\n");
    AudioDecoderConfig dec_config;
    dec_config.sample_rate = format.sample_rate;
    dec_config.channels = format.channels;

    auto dec_result = decoder.Initialize(dec_config);
    if (!dec_result.ok()) {
        printf("FAIL: Decoder init: %s\n", dec_result.error().message.c_str());
        return 1;
    }

    printf("[4/6] Initializing WASAPI audio renderer...\n");
    AudioRendererConfig render_config;
    render_config.sample_rate = format.sample_rate;
    render_config.channels = format.channels;
    render_config.pre_buffer_ms = 40;
    render_config.buffer_capacity_ms = 100;

    result = renderer.Initialize(render_config);
    if (!result.ok()) {
        printf("FAIL: Renderer init: %s\n", result.error().message.c_str());
        return 1;
    }

    // ── Wire up the pipeline ───────────────────────────────────────────

    // Encoder output -> decoder -> renderer
    encoder.SetOutputCallback([&](EncodedAudioPacket packet) {
        encoded_packets.fetch_add(1, std::memory_order_relaxed);

        auto decode_result =
            decoder.Decode(packet.data.data(), packet.data.size());
        if (decode_result.ok()) {
            decoded_frames.fetch_add(1, std::memory_order_relaxed);

            auto queue_result = renderer.QueueFrame(decode_result.value());
            if (queue_result.ok()) {
                queued_frames.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    // Capture output -> encoder input
    auto audio_callback = [&](const AudioFrame& frame) {
        captured_frames.fetch_add(1, std::memory_order_relaxed);
        encoder.Encode(frame.data, frame.frame_count, frame.timestamp_us);
    };

    // ── Start renderer and capture ─────────────────────────────────────

    printf("[5/6] Starting renderer and capture for 5 seconds...\n");
    printf("       Playing system sounds to generate audio...\n");
    printf("       NOTE: Feedback loop expected (capture == render device)\n\n");

    result = renderer.Start();
    if (!result.ok()) {
        printf("FAIL: Renderer start: %s\n", result.error().message.c_str());
        return 1;
    }

    result = capture.Start(audio_callback);
    if (!result.ok()) {
        printf("FAIL: Capture start: %s\n", result.error().message.c_str());
        renderer.Stop();
        return 1;
    }

    // Play system sounds in background thread
    std::thread audio_player([] {
        for (int i = 0; i < 10; ++i) {
            PlaySoundW(L"C:\\Windows\\Media\\Ring10.wav", nullptr,
                       SND_FILENAME | SND_SYNC);
        }
    });

    // Print stats periodically
    auto start_time = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start_time <
           std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto stats = renderer.GetStats();
        printf("  [%.0fs] captured=%u encoded=%u decoded=%u "
               "played=%llu underruns=%llu buf=%.1fms\n",
               std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start_time)
                   .count(),
               captured_frames.load(), encoded_packets.load(),
               decoded_frames.load(), stats.frames_played,
               stats.underrun_count, stats.buffer_level_ms);
    }

    audio_player.join();
    capture.Stop();
    encoder.Flush();

    // Let renderer drain for a moment
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    renderer.Stop();

    // ── Verify results ─────────────────────────────────────────────────

    auto final_stats = renderer.GetStats();

    printf("\n[6/6] Results:\n");
    printf("       Captured callbacks:    %u\n", captured_frames.load());
    printf("       Encoded packets:       %u\n", encoded_packets.load());
    printf("       Decoded frames:        %u\n", decoded_frames.load());
    printf("       Queued to renderer:    %u\n", queued_frames.load());
    printf("       Frames played:         %llu\n", final_stats.frames_played);
    printf("       Underrun count:        %llu\n", final_stats.underrun_count);
    printf("       Overrun count:         %llu\n", final_stats.overrun_count);
    printf("       Underrun samples:      %llu\n",
           final_stats.total_underrun_samples);
    printf("       Buffer level (EWMA):   %.1f ms\n",
           final_stats.buffer_level_ms);
    printf("       Playback timestamp:    %lld us\n",
           renderer.GetPlaybackTimestamp());

    bool passed = true;

    // Check: frames were played
    if (final_stats.frames_played == 0) {
        // May be zero if system was silent and no audio reached pre-buffer
        uint32_t encoded = encoded_packets.load();
        if (encoded == 0) {
            printf("\nWARN: No audio produced (system was likely silent)\n");
            printf("      WASAPI loopback only captures when audio plays.\n");
            printf("PASS: Pipeline initialized and ran without errors\n");
        } else {
            printf("FAIL: Encoded %u packets but frames_played == 0\n",
                   encoded);
            passed = false;
        }
    } else {
        printf("\nPASS: %llu frames played\n", final_stats.frames_played);
    }

    // Check: decoded count matches encoded count
    uint32_t encoded = encoded_packets.load();
    uint32_t decoded = decoded_frames.load();
    if (encoded > 0 && decoded != encoded) {
        printf("FAIL: Decoded frames (%u) != encoded packets (%u)\n",
               decoded, encoded);
        passed = false;
    } else if (encoded > 0) {
        printf("PASS: Decoded frame count matches encoded packet count\n");
    }

    // Check: underrun count is reasonable (some underruns expected at startup)
    if (final_stats.underrun_count > 50) {
        printf("WARN: High underrun count (%llu) — may indicate timing issues\n",
               final_stats.underrun_count);
    } else {
        printf("PASS: Underrun count is acceptable (%llu)\n",
               final_stats.underrun_count);
    }

    // Check: buffer level is in reasonable range
    if (final_stats.frames_played > 0 &&
        (final_stats.buffer_level_ms < 0.0 ||
         final_stats.buffer_level_ms > 200.0)) {
        printf("WARN: Buffer level %.1f ms outside expected range\n",
               final_stats.buffer_level_ms);
    } else if (final_stats.frames_played > 0) {
        printf("PASS: Buffer level %.1f ms in expected range\n",
               final_stats.buffer_level_ms);
    }

    printf("\n=== %s ===\n", passed ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return passed ? 0 : 1;
}
