/// End-to-end test for the Windows zero-copy decode -> render pipeline.
///
/// Reads a raw H.264 bitstream (output.h264 from capture_encode_test),
/// hardware-decodes it to GPU textures via Media Foundation, and renders
/// the decoded frames to a window via DXGI SwapChain.
///
/// Reports per-frame and aggregate latency statistics.

#include "common/d3d11_device_context.h"
#include "codec/d3d11_color_converter.h"
#include "codec/mf_video_decoder.h"
#include "renderer/d3d11_swapchain_renderer.h"

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
    double decode_ms;
    double render_ms;   // Color convert + copy to back buffer
    double present_ms;  // SwapChain Present (includes vsync wait)
    double total_ms;    // End-to-end for this frame
};

// ---------------------------------------------------------------------------
// H.264 Annex-B NAL unit parser
// ---------------------------------------------------------------------------

struct NalUnit {
    size_t offset;           // Byte offset of the start code in the bitstream.
    size_t length;           // Total length including start code.
    uint8_t start_code_size; // 3 or 4.
};

/// Find all NAL unit boundaries in a raw Annex-B bitstream.
/// Detects both 3-byte (00 00 01) and 4-byte (00 00 00 01) start codes.
static std::vector<NalUnit> FindNalUnits(
    const uint8_t* data, size_t size) {

    struct StartCode {
        size_t offset;
        uint8_t size;
    };
    std::vector<StartCode> start_codes;

    for (size_t i = 0; i + 2 < size; ++i) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                // Could be 3-byte or 4-byte start code.
                if (i > 0 && data[i - 1] == 0x00) {
                    // 4-byte: the previous byte was also 0x00.
                    // Already recorded as 3-byte at i-1 — replace it.
                    if (!start_codes.empty() && start_codes.back().offset == i - 1) {
                        start_codes.back().offset = i - 1;
                        start_codes.back().size = 4;
                    } else {
                        start_codes.push_back({i - 1, 4});
                    }
                } else {
                    start_codes.push_back({i, 3});
                }
                i += 2;  // Skip past start code.
            } else if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                start_codes.push_back({i, 4});
                i += 3;  // Skip past start code.
            }
        }
    }

    std::vector<NalUnit> nals;
    for (size_t i = 0; i < start_codes.size(); ++i) {
        size_t end = (i + 1 < start_codes.size()) ? start_codes[i + 1].offset : size;
        nals.push_back({start_codes[i].offset,
                        end - start_codes[i].offset,
                        start_codes[i].size});
    }

    return nals;
}

/// Group NAL units into access units (one frame per AU).
/// Returns vectors of NAL index ranges [start, end) for each AU.
///
/// AU boundary rules:
///   - SPS (type 7) always starts a new AU.
///   - PPS (type 8), SEI (type 6), and other non-VCL NALs attach to current AU.
///   - The first VCL NAL (types 1-5) after any non-VCL NAL starts a new AU.
///   - Consecutive VCL NALs belong to the same AU (multi-slice frames).
static std::vector<std::pair<size_t, size_t>> GroupAccessUnits(
    const uint8_t* data,
    const std::vector<NalUnit>& nals) {

    std::vector<size_t> au_starts;
    bool last_was_vcl = false;

    for (size_t i = 0; i < nals.size(); ++i) {
        uint8_t nal_type = data[nals[i].offset + nals[i].start_code_size] & 0x1F;

        bool is_vcl = (nal_type >= 1 && nal_type <= 5);

        if (nal_type == 7) {
            // SPS always opens a new AU.
            au_starts.push_back(i);
            last_was_vcl = false;
        } else if (is_vcl) {
            if (!last_was_vcl) {
                // First VCL after non-VCL — new AU (unless SPS already started one
                // that contains this NAL as its first VCL).
                if (au_starts.empty()) {
                    au_starts.push_back(i);
                } else {
                    // Check: did a non-VCL NAL already start this AU?
                    // If the current AU has no VCL yet, this slice belongs to it.
                    // Otherwise, start a new AU.
                    bool current_au_has_vcl = false;
                    for (size_t j = au_starts.back(); j < i; ++j) {
                        uint8_t jt = data[nals[j].offset + nals[j].start_code_size] & 0x1F;
                        if (jt >= 1 && jt <= 5) {
                            current_au_has_vcl = true;
                            break;
                        }
                    }
                    if (current_au_has_vcl) {
                        au_starts.push_back(i);
                    }
                }
            }
            // Consecutive VCL NALs stay in same AU (multi-slice).
            last_was_vcl = true;
        } else {
            // Non-VCL, non-SPS (PPS, SEI, etc.): attach to current AU.
            if (au_starts.empty()) {
                au_starts.push_back(i);
            }
            last_was_vcl = false;
        }
    }

    // Build AU ranges.
    std::vector<std::pair<size_t, size_t>> aus;
    for (size_t i = 0; i < au_starts.size(); ++i) {
        size_t start = au_starts[i];
        size_t end = (i + 1 < au_starts.size()) ? au_starts[i + 1] : nals.size();
        aus.emplace_back(start, end);
    }

    return aus;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

static void PrintStats(const std::vector<FrameTiming>& timings) {
    if (timings.empty()) {
        printf("No frames decoded.\n");
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

    std::vector<double> decodes, renders, presents, totals;
    for (auto& t : timings) {
        decodes.push_back(t.decode_ms);
        renders.push_back(t.render_ms);
        presents.push_back(t.present_ms);
        totals.push_back(t.total_ms);
    }

    printf("\n=== Pipeline Performance (%zu frames) ===\n", timings.size());
    printf("%-20s %8s %8s %8s %8s\n", "Stage", "Avg", "P50", "P95", "P99");
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Decode",
           avg(decodes), percentile(decodes, 50),
           percentile(decodes, 95), percentile(decodes, 99));
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Render (CSC)",
           avg(renders), percentile(renders, 50),
           percentile(renders, 95), percentile(renders, 99));
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Present",
           avg(presents), percentile(presents, 50),
           percentile(presents, 95), percentile(presents, 99));
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", "Total",
           avg(totals), percentile(totals, 50),
           percentile(totals, 95), percentile(totals, 99));

    double total_time_s = 0;
    for (auto& t : totals) total_time_s += t;
    total_time_s /= 1000.0;
    printf("\nEffective FPS: %.1f\n", timings.size() / total_time_s);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    printf("Lumen Zero-Copy Decode & Render Test\n");
    printf("====================================\n\n");

    // Initialize COM and Media Foundation.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    constexpr uint32_t kOutputIndex = 0;
    const char* kInputFile = "output.h264";

    // 1. Read the H.264 bitstream.
    printf("[1/5] Reading H.264 bitstream...\n");
    std::ifstream file(kInputFile, std::ios::binary | std::ios::ate);
    if (!file) {
        printf("FAILED: Could not open %s\n", kInputFile);
        printf("Run capture_encode_test first to generate this file.\n");
        return 1;
    }

    size_t file_size = static_cast<size_t>(file.tellg());
    file.seekg(0);
    std::vector<uint8_t> bitstream(file_size);
    file.read(reinterpret_cast<char*>(bitstream.data()), file_size);
    file.close();
    printf("  OK - %.2f MB\n", file_size / (1024.0 * 1024.0));

    // 2. Parse NAL units and group into access units.
    printf("[2/5] Parsing H.264 bitstream...\n");
    auto nals = FindNalUnits(bitstream.data(), bitstream.size());
    printf("  Found %zu NAL units\n", nals.size());

    auto access_units = GroupAccessUnits(bitstream.data(), nals);
    printf("  Grouped into %zu access units (frames)\n", access_units.size());

    if (access_units.empty()) {
        printf("FAILED: No access units found in bitstream\n");
        return 1;
    }

    // Determine resolution from the bitstream filename convention.
    // Default to 1920x1080; actual resolution will be negotiated by the decoder.
    uint32_t width = 1920;
    uint32_t height = 1080;

    // 3. Create shared D3D11 device.
    printf("[3/5] Creating D3D11 device...\n");
    D3D11DeviceContext device_ctx;
    auto result = device_ctx.Initialize(kOutputIndex);
    if (!result) {
        printf("FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  OK\n");

    // 4. Initialize decoder, color converter, and renderer.
    printf("[4/5] Initializing pipeline...\n");

    // Decoder
    MfVideoDecoder decoder(device_ctx);
    VideoDecoderConfig dec_config;
    dec_config.codec = VideoCodec::kH264;
    dec_config.width = width;
    dec_config.height = height;
    dec_config.fps = 60;
    dec_config.low_latency = true;

    result = decoder.Initialize(dec_config, nullptr);
    if (!result) {
        printf("  Decoder FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  Decoder: OK (H.264 %ux%u, low-latency)\n", width, height);

    // Color converter (NV12 → BGRA for swap chain)
    D3D11ColorConverter converter(device_ctx);
    VideoFrameDesc nv12_desc;
    nv12_desc.width = width;
    nv12_desc.height = height;
    nv12_desc.format = PixelFormat::kNV12;

    result = converter.Initialize(nv12_desc, PixelFormat::kBGRA, nullptr);
    if (!result) {
        printf("  Color converter FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  Color converter: OK (NV12 -> BGRA)\n");

    // Renderer
    D3D11SwapChainRenderer renderer(device_ctx, converter);
    result = renderer.Initialize(width, height, nullptr);
    if (!result) {
        printf("  Renderer FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  Renderer: OK (SwapChain %ux%u)\n", width, height);

    // 5. Decode and render loop.
    printf("[5/5] Decoding and rendering %zu frames...\n\n",
           access_units.size());

    std::vector<FrameTiming> timings;
    timings.reserve(access_units.size());
    uint32_t decode_errors = 0;
    uint32_t needs_more_input = 0;
    uint32_t frames_decoded = 0;

    for (size_t au_idx = 0; au_idx < access_units.size(); ++au_idx) {
        if (renderer.IsWindowClosed()) {
            printf("Window closed by user.\n");
            break;
        }

        auto frame_start = Clock::now();

        // Gather all NAL units in this access unit into a contiguous buffer.
        auto [nal_start, nal_end] = access_units[au_idx];
        size_t au_offset = nals[nal_start].offset;
        size_t au_end_offset = nals[nal_end - 1].offset + nals[nal_end - 1].length;
        size_t au_size = au_end_offset - au_offset;

        // Decode
        auto decode_result = decoder.Decode(
            bitstream.data() + au_offset, au_size, au_idx);
        auto after_decode = Clock::now();

        if (!decode_result) {
            if (decode_result.error().code == ErrorCode::kTimeout) {
                // Decoder needs more input — normal for first few frames.
                needs_more_input++;
                continue;
            }
            printf("Decode error (frame %zu): %s\n",
                   au_idx, decode_result.error().message.c_str());
            decode_errors++;
            if (decode_errors > 10) {
                printf("Too many decode errors, aborting.\n");
                break;
            }
            continue;
        }

        auto& decoded_frame = decode_result.value();
        frames_decoded++;

        // Render (NV12 → SwapChain via VideoProcessorBlt)
        auto render_start = Clock::now();
        result = renderer.RenderFrame(decoded_frame.native_texture,
                                       decoded_frame.format);
        auto after_render = Clock::now();

        decoder.ReleaseFrame(decoded_frame);

        if (!result) {
            printf("Render error (frame %zu): %s\n",
                   au_idx, result.error().message.c_str());
            continue;
        }

        // Present
        auto present_start = Clock::now();
        result = renderer.Present();
        auto after_present = Clock::now();

        if (!result) {
            printf("Present error (frame %zu): %s\n",
                   au_idx, result.error().message.c_str());
            continue;
        }

        // Record timings.
        FrameTiming t;
        t.decode_ms = std::chrono::duration<double, std::milli>(
            after_decode - frame_start).count();
        t.render_ms = std::chrono::duration<double, std::milli>(
            after_render - render_start).count();
        t.present_ms = std::chrono::duration<double, std::milli>(
            after_present - present_start).count();
        t.total_ms = std::chrono::duration<double, std::milli>(
            after_present - frame_start).count();

        timings.push_back(t);

        // Progress indicator every 60 frames.
        if (timings.size() % 60 == 0) {
            printf("  %zu/%zu frames (%.1f ms last)\n",
                   timings.size(), access_units.size(), t.total_ms);
        }
    }

    // Flush decoder.
    decoder.Flush();

    // Print results.
    PrintStats(timings);

    printf("\nDecoded: %u frames rendered, %u needed-more-input, %u errors\n",
           frames_decoded, needs_more_input, decode_errors);
    printf("Total access units in bitstream: %zu\n", access_units.size());

    // Cleanup.
    MFShutdown();
    CoUninitialize();

    return timings.empty() ? 1 : 0;
}
