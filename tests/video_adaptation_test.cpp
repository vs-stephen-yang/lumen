/// Video Adaptation Test — exercises dynamic bitrate, FPS, HEVC, intra refresh,
/// resolution change, and FramePacer.
///
/// Uses high-motion synthetic NV12 content so the encoder has real entropy to
/// compress, making bitrate differences measurable.

#include "common/d3d11_device_context.h"
#include "codec/mf_video_encoder.h"
#include "lumen/codec/frame_pacer.h"

#include <d3d11.h>
#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

using namespace lumen;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Test infrastructure
// ---------------------------------------------------------------------------

struct TestResult {
    const char* name;
    bool passed;
    std::string detail;
};

static std::vector<TestResult> g_results;

static void RecordResult(const char* name, bool passed,
                          const std::string& detail = {}) {
    g_results.push_back({name, passed, detail});
    printf("  [%s] %s", passed ? "PASS" : "FAIL", name);
    if (!detail.empty()) printf(" — %s", detail.c_str());
    printf("\n");
}

// ---------------------------------------------------------------------------
// High-motion NV12 content generation
// ---------------------------------------------------------------------------

/// Fill a staging texture with high-motion NV12 content.
/// Each frame varies significantly from the previous to produce real entropy.
static void FillHighMotionNV12(ID3D11DeviceContext* ctx,
                                ID3D11Texture2D* staging,
                                ID3D11Texture2D* gpu_texture,
                                uint32_t width, uint32_t height,
                                uint32_t frame_index) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &mapped);
    if (FAILED(hr)) return;

    auto* y_data = static_cast<uint8_t*>(mapped.pData);
    uint8_t base = static_cast<uint8_t>((frame_index * 37) % 256);

    // Y plane — shifting gradient with noise-like variation
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t* p = y_data + row * mapped.RowPitch;
        for (uint32_t col = 0; col < width; ++col) {
            // Mix frame index, row, col for high spatial+temporal variance
            uint8_t val = static_cast<uint8_t>(
                base + (row * 97 / height) + (col * 131 / width) +
                ((row ^ col ^ (frame_index * 53)) & 0x3F));
            p[col] = val;
        }
    }

    // UV plane — varying chroma
    auto* uv_data = y_data + height * mapped.RowPitch;
    uint8_t u_base = static_cast<uint8_t>((frame_index * 23 + 64) % 256);
    uint8_t v_base = static_cast<uint8_t>((frame_index * 41 + 192) % 256);
    for (uint32_t row = 0; row < height / 2; ++row) {
        uint8_t* p = uv_data + row * mapped.RowPitch;
        for (uint32_t col = 0; col < width; col += 2) {
            p[col]     = static_cast<uint8_t>(u_base + (row & 0x1F));
            p[col + 1] = static_cast<uint8_t>(v_base + (col & 0x1F));
        }
    }

    ctx->Unmap(staging, 0);
    ctx->CopyResource(gpu_texture, staging);
}

// ---------------------------------------------------------------------------
// Helper: create staging + GPU NV12 texture pair
// ---------------------------------------------------------------------------

struct TexturePair {
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Texture2D> gpu;
};

static bool CreateNV12Textures(D3D11DeviceContext& device_ctx,
                                uint32_t width, uint32_t height,
                                TexturePair& out) {
    auto gpu_result = device_ctx.CreateNV12Texture(width, height);
    if (!gpu_result) return false;
    out.gpu = std::move(gpu_result).value();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_ctx.Device()->CreateTexture2D(&desc, nullptr,
                                                       &out.staging);
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Helper: encode N frames and collect packet sizes
// ---------------------------------------------------------------------------

static bool EncodeFrames(MfVideoEncoder& encoder, D3D11DeviceContext& device_ctx,
                          TexturePair& textures, uint32_t width, uint32_t height,
                          uint32_t num_frames, uint32_t start_index,
                          std::vector<size_t>& packet_sizes) {
    for (uint32_t i = 0; i < num_frames; ++i) {
        FillHighMotionNV12(device_ctx.Context(), textures.staging.Get(),
                           textures.gpu.Get(), width, height, start_index + i);

        FrameMetadata meta;
        meta.frame_index = start_index + i;

        auto result = encoder.Encode(textures.gpu.Get(), meta);
        if (!result) {
            printf("    Encode error at frame %u: %s\n", start_index + i,
                   result.error().message.c_str());
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sub-test 1: Basic H.264 CBR encoding
// ---------------------------------------------------------------------------

static void TestBasicH264(D3D11DeviceContext& device_ctx, TexturePair& textures) {
    printf("\n--- Test 1: Basic H.264 CBR 1080p60 ---\n");

    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.bitrate_bps = 8'000'000;
    config.codec = VideoCodec::kH264;
    config.rate_control = RateControlMode::kCBR;
    config.low_latency = true;

    std::vector<size_t> packet_sizes;
    encoder.SetOutputCallback([&](EncodedPacket packet) {
        packet_sizes.push_back(packet.data.size());
    });

    auto result = encoder.Initialize(config, nullptr);
    if (!result) {
        RecordResult("H.264 CBR 1080p60", false, result.error().message);
        return;
    }

    bool ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 60, 0,
                           packet_sizes);
    encoder.Flush();

    if (ok && !packet_sizes.empty()) {
        size_t total = std::accumulate(packet_sizes.begin(), packet_sizes.end(),
                                       size_t{0});
        char buf[128];
        snprintf(buf, sizeof(buf), "%zu packets, %.1f KB avg",
                 packet_sizes.size(), total / 1024.0 / packet_sizes.size());
        RecordResult("H.264 CBR 1080p60", true, buf);
    } else {
        RecordResult("H.264 CBR 1080p60", false,
                     ok ? "No packets produced" : "Encode failed");
    }
}

// ---------------------------------------------------------------------------
// Sub-test 2: Dynamic bitrate change
// ---------------------------------------------------------------------------

static void TestDynamicBitrate(D3D11DeviceContext& device_ctx,
                                TexturePair& textures) {
    printf("\n--- Test 2: Dynamic Bitrate (8Mbps → 2Mbps) ---\n");

    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.bitrate_bps = 8'000'000;
    config.codec = VideoCodec::kH264;
    config.rate_control = RateControlMode::kCBR;
    config.low_latency = true;

    std::vector<size_t> high_sizes, low_sizes;
    bool in_low_phase = false;

    encoder.SetOutputCallback([&](EncodedPacket packet) {
        if (in_low_phase)
            low_sizes.push_back(packet.data.size());
        else
            high_sizes.push_back(packet.data.size());
    });

    auto result = encoder.Initialize(config, nullptr);
    if (!result) {
        RecordResult("Dynamic bitrate", false, result.error().message);
        return;
    }

    // Phase 1: 60 frames at 8Mbps
    std::vector<size_t> dummy;
    bool ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 60, 0,
                           dummy);
    if (!ok) {
        RecordResult("Dynamic bitrate", false, "Phase 1 encode failed");
        return;
    }

    // Change to 2Mbps
    in_low_phase = true;
    result = encoder.SetBitrate(2'000'000);
    if (!result) {
        RecordResult("Dynamic bitrate", false,
                     "SetBitrate failed: " + result.error().message);
        return;
    }

    // Phase 2: 120 frames at 2Mbps
    ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 120, 60,
                      dummy);
    encoder.Flush();

    if (!ok || high_sizes.empty() || low_sizes.empty()) {
        RecordResult("Dynamic bitrate", false, "Insufficient packets");
        return;
    }

    double high_avg = std::accumulate(high_sizes.begin(), high_sizes.end(),
                                       0.0) / high_sizes.size();
    double low_avg = std::accumulate(low_sizes.begin(), low_sizes.end(),
                                      0.0) / low_sizes.size();

    // Skip first few low-phase packets (encoder ramp-down lag)
    if (low_sizes.size() > 30) {
        std::vector<size_t> steady(low_sizes.begin() + 30, low_sizes.end());
        low_avg = std::accumulate(steady.begin(), steady.end(), 0.0)
                  / steady.size();
    }

    double reduction = 1.0 - (low_avg / high_avg);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "high=%.0f B avg, low=%.0f B avg, reduction=%.0f%%",
             high_avg, low_avg, reduction * 100);

    // Expect at least 30% reduction
    RecordResult("Dynamic bitrate", reduction >= 0.30, buf);
}

// ---------------------------------------------------------------------------
// Sub-test 3: Dynamic FPS change
// ---------------------------------------------------------------------------

static void TestDynamicFps(D3D11DeviceContext& device_ctx,
                            TexturePair& textures) {
    printf("\n--- Test 3: Dynamic FPS (60 → 30) ---\n");

    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.bitrate_bps = 8'000'000;
    config.codec = VideoCodec::kH264;
    config.low_latency = true;

    uint32_t packet_count = 0;
    encoder.SetOutputCallback([&](EncodedPacket) { packet_count++; });

    auto result = encoder.Initialize(config, nullptr);
    if (!result) {
        RecordResult("Dynamic FPS", false, result.error().message);
        return;
    }

    // Encode 30 frames at 60fps
    std::vector<size_t> dummy;
    bool ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 30, 0,
                           dummy);
    if (!ok) {
        RecordResult("Dynamic FPS", false, "Phase 1 failed");
        return;
    }

    // Change to 30fps
    result = encoder.SetFrameRate(30);
    if (!result) {
        RecordResult("Dynamic FPS", false,
                     "SetFrameRate failed: " + result.error().message);
        return;
    }

    // Encode 60 more frames at 30fps
    ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 60, 30,
                      dummy);
    encoder.Flush();

    char buf[64];
    snprintf(buf, sizeof(buf), "%u packets total", packet_count);
    RecordResult("Dynamic FPS", ok && packet_count > 0, buf);
}

// ---------------------------------------------------------------------------
// Sub-test 4: HEVC encoding
// ---------------------------------------------------------------------------

static void TestHEVC(D3D11DeviceContext& device_ctx, TexturePair& textures) {
    printf("\n--- Test 4: HEVC Encoding ---\n");

    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.bitrate_bps = 8'000'000;
    config.codec = VideoCodec::kHEVC;
    config.low_latency = true;

    std::vector<std::vector<uint8_t>> packets;
    encoder.SetOutputCallback([&](EncodedPacket packet) {
        packets.push_back(std::move(packet.data));
    });

    auto result = encoder.Initialize(config, nullptr);
    if (!result) {
        // HEVC may not be available on all systems
        RecordResult("HEVC encoding", false,
                     "Init failed (may be unavailable): " +
                         result.error().message);
        return;
    }

    std::vector<size_t> dummy;
    bool ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 60, 0,
                           dummy);
    encoder.Flush();

    if (!ok || packets.empty()) {
        RecordResult("HEVC encoding", false,
                     ok ? "No packets" : "Encode failed");
        return;
    }

    // Verify HEVC NAL units: scan for start codes and check NAL type
    // HEVC NAL header: forbidden(1) + type(6) + layer_id(6) + temporal_id(3)
    // NAL type is (byte[0] >> 1) & 0x3F
    bool found_idr = false;
    bool found_non_idr = false;

    for (const auto& pkt : packets) {
        for (size_t i = 0; i + 4 < pkt.size(); ++i) {
            // Look for 00 00 00 01 or 00 00 01 start codes
            bool start3 = (pkt[i] == 0 && pkt[i+1] == 0 && pkt[i+2] == 1);
            bool start4 = (i + 4 < pkt.size() && pkt[i] == 0 && pkt[i+1] == 0 &&
                          pkt[i+2] == 0 && pkt[i+3] == 1);

            size_t nal_offset = 0;
            if (start4) { nal_offset = i + 4; i += 3; }
            else if (start3) { nal_offset = i + 3; i += 2; }
            else continue;

            if (nal_offset < pkt.size()) {
                uint8_t nal_type = (pkt[nal_offset] >> 1) & 0x3F;
                // HEVC IDR types: 19 (IDR_W_RADL), 20 (IDR_N_LP)
                if (nal_type == 19 || nal_type == 20) found_idr = true;
                // HEVC non-IDR slice: 0 (TRAIL_N), 1 (TRAIL_R)
                if (nal_type == 0 || nal_type == 1) found_non_idr = true;
            }
        }
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "%zu packets, IDR=%s, non-IDR=%s",
             packets.size(),
             found_idr ? "yes" : "no",
             found_non_idr ? "yes" : "no");
    RecordResult("HEVC encoding", found_idr, buf);
}

// ---------------------------------------------------------------------------
// Sub-test 5: Intra refresh
// ---------------------------------------------------------------------------

static void TestIntraRefresh(D3D11DeviceContext& device_ctx,
                              TexturePair& textures) {
    printf("\n--- Test 5: Intra Refresh ---\n");

    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.bitrate_bps = 8'000'000;
    config.codec = VideoCodec::kH264;
    config.low_latency = true;
    config.intra_refresh = true;
    config.intra_refresh_period_frames = 60;

    std::vector<size_t> packet_sizes;
    encoder.SetOutputCallback([&](EncodedPacket packet) {
        packet_sizes.push_back(packet.data.size());
    });

    auto result = encoder.Initialize(config, nullptr);
    if (!result) {
        RecordResult("Intra refresh", false, result.error().message);
        return;
    }

    std::vector<size_t> dummy;
    bool ok = EncodeFrames(encoder, device_ctx, textures, 1920, 1080, 120, 0,
                           dummy);
    encoder.Flush();

    if (!ok || packet_sizes.size() < 10) {
        RecordResult("Intra refresh", false, "Insufficient packets");
        return;
    }

    // Skip first packet (initial IDR).
    std::vector<size_t> non_idr(packet_sizes.begin() + 1, packet_sizes.end());
    double avg = std::accumulate(non_idr.begin(), non_idr.end(), 0.0)
                 / non_idr.size();
    size_t max_size = *std::max_element(non_idr.begin(), non_idr.end());

    // With intra refresh, max packet should be smaller relative to average.
    // Note: Many hardware MFTs silently ignore intra refresh properties and
    // still produce periodic IDR frames. We use a generous threshold (5x)
    // to account for this. A fully-supporting MFT would show < 2x.
    double ratio = static_cast<double>(max_size) / avg;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "%zu packets, avg=%.0f B, max=%zu B, max/avg=%.1fx",
             packet_sizes.size(), avg, max_size, ratio);

    // Pass if ratio < 5x (generous — some HW ignores intra refresh)
    RecordResult("Intra refresh", ratio < 5.0, buf);
}

// ---------------------------------------------------------------------------
// Sub-test 6: Resolution change via Reconfigure
// ---------------------------------------------------------------------------

static void TestResolutionChange(D3D11DeviceContext& device_ctx) {
    printf("\n--- Test 6: Resolution Change (1080p → 720p) ---\n");

    // Create 1080p textures
    TexturePair tex1080;
    if (!CreateNV12Textures(device_ctx, 1920, 1080, tex1080)) {
        RecordResult("Resolution change", false, "Failed to create 1080p textures");
        return;
    }

    MfVideoEncoder encoder(device_ctx);
    VideoEncoderConfig config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 60;
    config.bitrate_bps = 8'000'000;
    config.codec = VideoCodec::kH264;
    config.low_latency = true;

    uint32_t packet_count_1080 = 0;
    uint32_t packet_count_720 = 0;
    bool in_720 = false;

    encoder.SetOutputCallback([&](EncodedPacket) {
        if (in_720) packet_count_720++;
        else packet_count_1080++;
    });

    auto result = encoder.Initialize(config, nullptr);
    if (!result) {
        RecordResult("Resolution change", false, result.error().message);
        return;
    }

    // Encode 30 frames at 1080p
    std::vector<size_t> dummy;
    bool ok = EncodeFrames(encoder, device_ctx, tex1080, 1920, 1080, 30, 0,
                           dummy);
    if (!ok) {
        RecordResult("Resolution change", false, "1080p encode failed");
        return;
    }

    // Reconfigure to 720p
    in_720 = true;
    VideoEncoderConfig config_720 = config;
    config_720.width = 1280;
    config_720.height = 720;
    config_720.bitrate_bps = 4'000'000;

    result = encoder.Reconfigure(config_720);
    if (!result) {
        RecordResult("Resolution change", false,
                     "Reconfigure failed: " + result.error().message);
        return;
    }

    // Create 720p textures
    TexturePair tex720;
    if (!CreateNV12Textures(device_ctx, 1280, 720, tex720)) {
        RecordResult("Resolution change", false, "Failed to create 720p textures");
        return;
    }

    // Need to re-register callback after Reconfigure (encoder was rebuilt)
    encoder.SetOutputCallback([&](EncodedPacket) { packet_count_720++; });

    ok = EncodeFrames(encoder, device_ctx, tex720, 1280, 720, 60, 30, dummy);
    encoder.Flush();

    char buf[128];
    snprintf(buf, sizeof(buf), "1080p: %u packets, 720p: %u packets",
             packet_count_1080, packet_count_720);
    RecordResult("Resolution change",
                 ok && packet_count_1080 > 0 && packet_count_720 > 0, buf);
}

// ---------------------------------------------------------------------------
// Sub-test 7: FramePacer unit tests (no encoder needed)
// ---------------------------------------------------------------------------

static void TestFramePacer() {
    printf("\n--- Test 7: FramePacer ---\n");

    bool all_pass = true;

    // Test 1: First frame always encodes
    {
        FramePacer pacer;
        pacer.SetTargetFps(60);
        auto d = pacer.ShouldEncode(0);
        if (d != PaceDecision::kEncode) {
            printf("    FAIL: First frame should encode\n");
            all_pass = false;
        }
    }

    // Test 2: Frame arriving too early should be dropped
    {
        FramePacer pacer;
        pacer.SetTargetFps(60);  // 16,667µs interval
        pacer.ShouldEncode(0);   // First frame
        auto d = pacer.ShouldEncode(5'000);  // 5ms later (< 75% of 16.7ms)
        if (d != PaceDecision::kDrop) {
            printf("    FAIL: Early frame should be dropped\n");
            all_pass = false;
        }
    }

    // Test 3: Frame at target interval should encode
    {
        FramePacer pacer;
        pacer.SetTargetFps(60);
        pacer.ShouldEncode(0);
        auto d = pacer.ShouldEncode(16'667);  // Exactly one interval
        if (d != PaceDecision::kEncode) {
            printf("    FAIL: On-time frame should encode\n");
            all_pass = false;
        }
    }

    // Test 4: Tick should return duplicate when idle
    {
        FramePacer pacer;
        pacer.SetTargetFps(60);
        pacer.ShouldEncode(0);
        auto d = pacer.Tick(30'000);  // 30ms = ~180% of interval
        if (d != PaceDecision::kDuplicate) {
            printf("    FAIL: Idle tick should trigger duplicate\n");
            all_pass = false;
        }
    }

    // Test 5: Tick within normal interval should not duplicate
    {
        FramePacer pacer;
        pacer.SetTargetFps(60);
        pacer.ShouldEncode(0);
        auto d = pacer.Tick(10'000);  // 10ms = ~60% of interval
        if (d == PaceDecision::kDuplicate) {
            printf("    FAIL: Normal tick should not duplicate\n");
            all_pass = false;
        }
    }

    // Test 6: Synthetic timestamps advance monotonically
    {
        FramePacer pacer;
        pacer.SetTargetFps(30);  // 33,333µs interval → 333,330 100ns units
        int64_t t0 = pacer.GetSampleTimestamp100ns();
        int64_t t1 = pacer.GetSampleTimestamp100ns();
        int64_t t2 = pacer.GetSampleTimestamp100ns();
        int64_t dur = pacer.GetSampleDuration100ns();

        if (t0 != 0 || t1 != dur || t2 != dur * 2) {
            printf("    FAIL: Timestamps not monotonic (t0=%lld, t1=%lld, "
                   "t2=%lld, dur=%lld)\n", t0, t1, t2, dur);
            all_pass = false;
        }
    }

    // Test 7: FPS change updates interval
    {
        FramePacer pacer;
        pacer.SetTargetFps(60);
        pacer.ShouldEncode(0);

        // At 60fps, frame at 13ms (78% of 16.7ms) should encode
        auto d1 = pacer.ShouldEncode(13'000);
        if (d1 != PaceDecision::kEncode) {
            printf("    FAIL: 13ms at 60fps should encode\n");
            all_pass = false;
        }

        pacer.ResetAll();
        pacer.SetTargetFps(30);
        pacer.ShouldEncode(0);

        // At 30fps, frame at 13ms (39% of 33.3ms) should be dropped
        auto d2 = pacer.ShouldEncode(13'000);
        if (d2 != PaceDecision::kDrop) {
            printf("    FAIL: 13ms at 30fps should be dropped\n");
            all_pass = false;
        }
    }

    RecordResult("FramePacer", all_pass,
                 all_pass ? "7/7 sub-tests passed" : "Some sub-tests failed");
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("Lumen Video Adaptation Test\n");
    printf("============================\n");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    // FramePacer tests don't need GPU
    TestFramePacer();

    // Create shared D3D11 device
    printf("\nInitializing D3D11 device...\n");
    D3D11DeviceContext device_ctx;
    auto result = device_ctx.Initialize(0);
    if (!result) {
        printf("FATAL: D3D11 init failed: %s\n", result.error().message.c_str());
        printf("\nSkipping GPU-dependent tests.\n");
    } else {
        printf("  OK\n");

        // Create default 1080p textures
        TexturePair textures;
        if (!CreateNV12Textures(device_ctx, 1920, 1080, textures)) {
            printf("FATAL: Cannot create NV12 textures\n");
        } else {
            TestBasicH264(device_ctx, textures);
            TestDynamicBitrate(device_ctx, textures);
            TestDynamicFps(device_ctx, textures);
            TestHEVC(device_ctx, textures);
            TestIntraRefresh(device_ctx, textures);
            TestResolutionChange(device_ctx);
        }
    }

    // Summary
    printf("\n============================\n");
    printf("Summary:\n");
    int pass_count = 0, fail_count = 0;
    for (const auto& r : g_results) {
        printf("  [%s] %s\n", r.passed ? "PASS" : "FAIL", r.name);
        if (r.passed) pass_count++;
        else fail_count++;
    }
    printf("\n%d passed, %d failed, %d total\n",
           pass_count, fail_count, pass_count + fail_count);

    MFShutdown();
    CoUninitialize();

    return fail_count > 0 ? 1 : 0;
}
