/// Standalone encoder test -- verifies the MF hardware encoder works
/// by feeding synthetic NV12 frames from a D3D11 texture.
///
/// Does NOT require DXGI Desktop Duplication (works in RDP sessions).
/// Writes raw H.264 bitstream to output.h264 for verification.

#include "common/d3d11_device_context.h"
#include "codec/mf_video_encoder.h"

#include <d3d11.h>
#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <vector>

using namespace lumen;
using Clock = std::chrono::high_resolution_clock;

int main() {
    // Line-buffered stdout so we see output before any potential hang.
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("Lumen Encoder Test (Synthetic NV12)\n");
    printf("====================================\n\n");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    constexpr uint32_t kWidth = 1920;
    constexpr uint32_t kHeight = 1080;
    constexpr uint32_t kFps = 60;
    constexpr uint32_t kNumFrames = 300;

    // 1. Create D3D11 device.
    printf("[1/3] Creating D3D11 device...\n");
    D3D11DeviceContext device_ctx;
    auto result = device_ctx.Initialize(0);
    if (!result) {
        printf("FAILED: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  OK\n");

    // 2. Create NV12 texture.
    printf("[2/3] Creating NV12 texture (%ux%u)...\n", kWidth, kHeight);
    auto nv12_result = device_ctx.CreateNV12Texture(kWidth, kHeight);
    if (!nv12_result) {
        printf("FAILED: %s\n", nv12_result.error().message.c_str());
        return 1;
    }
    auto nv12_texture = std::move(nv12_result).value();

    // Also create a CPU-writable staging texture for uploading NV12 data.
    // NV12 staging may not work on all drivers, so fall back to just using
    // the GPU texture directly (it will contain uninitialized data, which is
    // fine for testing the encoder pipeline).
    ComPtr<ID3D11Texture2D> staging_texture;
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = device_ctx.Device()->CreateTexture2D(
            &desc, nullptr, &staging_texture);
        if (FAILED(hr)) {
            printf("  WARNING: Could not create NV12 staging texture (0x%08lx).\n"
                   "  Will encode uninitialized GPU data (pipeline test only).\n",
                   static_cast<unsigned long>(hr));
        }
    }
    printf("  OK\n");

    // 3. Initialize encoder.
    printf("[3/3] Initializing MF hardware encoder (H.264)...\n");
    MfVideoEncoder encoder(device_ctx);

    VideoEncoderConfig enc_config;
    enc_config.width = kWidth;
    enc_config.height = kHeight;
    enc_config.fps = kFps;
    enc_config.bitrate_bps = 8'000'000;
    enc_config.codec = VideoCodec::kH264;
    enc_config.low_latency = true;

    uint64_t total_encoded_bytes = 0;
    uint32_t keyframe_count = 0;
    uint32_t packet_count = 0;

    std::ofstream bitstream_file("output.h264", std::ios::binary);

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

    // 4. Encode loop.
    printf("\nEncoding %u synthetic frames...\n\n", kNumFrames);

    auto* raw_ctx = device_ctx.Context();

    std::vector<double> encode_times_ms;
    encode_times_ms.reserve(kNumFrames);

    for (uint32_t i = 0; i < kNumFrames; ++i) {
        // Optionally upload varying NV12 data.
        if (staging_texture) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = raw_ctx->Map(staging_texture.Get(), 0,
                                       D3D11_MAP_WRITE, 0, &mapped);
            if (SUCCEEDED(hr)) {
                uint8_t y_base = static_cast<uint8_t>((i * 3) % 200 + 16);
                auto* y_data = static_cast<uint8_t*>(mapped.pData);

                // Y plane
                for (uint32_t row = 0; row < kHeight; ++row) {
                    uint8_t* p = y_data + row * mapped.RowPitch;
                    for (uint32_t col = 0; col < kWidth; ++col) {
                        p[col] = static_cast<uint8_t>(
                            y_base + (row * 64 / kHeight) + (col * 64 / kWidth));
                    }
                }
                // UV plane
                auto* uv_data = y_data + kHeight * mapped.RowPitch;
                for (uint32_t row = 0; row < kHeight / 2; ++row) {
                    uint8_t* p = uv_data + row * mapped.RowPitch;
                    for (uint32_t col = 0; col < kWidth; col += 2) {
                        p[col]     = 128;
                        p[col + 1] = 128;
                    }
                }

                raw_ctx->Unmap(staging_texture.Get(), 0);
                raw_ctx->CopyResource(nv12_texture.Get(), staging_texture.Get());
            }
        }

        FrameMetadata meta;
        meta.frame_index = i;

        auto t0 = Clock::now();
        result = encoder.Encode(nv12_texture.Get(), meta);
        auto t1 = Clock::now();

        if (!result) {
            printf("Encode error at frame %u: %s\n", i,
                   result.error().message.c_str());
            break;
        }

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        encode_times_ms.push_back(ms);

        if ((i + 1) % 60 == 0) {
            printf("  %u/%u frames (%.1f ms last, %u packets)\n",
                   i + 1, kNumFrames, ms, packet_count);
        }
    }

    // Flush.
    printf("Flushing encoder...\n");
    encoder.Flush();
    bitstream_file.close();

    // Stats.
    if (!encode_times_ms.empty()) {
        std::sort(encode_times_ms.begin(), encode_times_ms.end());
        double avg = std::accumulate(encode_times_ms.begin(),
                                      encode_times_ms.end(), 0.0)
                     / encode_times_ms.size();
        double p50 = encode_times_ms[encode_times_ms.size() / 2];
        double p95 = encode_times_ms[static_cast<size_t>(
            0.95 * (encode_times_ms.size() - 1))];
        double p99 = encode_times_ms[static_cast<size_t>(
            0.99 * (encode_times_ms.size() - 1))];

        printf("\n=== Encode Performance (%zu frames) ===\n",
               encode_times_ms.size());
        printf("  Avg: %.2f ms\n", avg);
        printf("  P50: %.2f ms\n", p50);
        printf("  P95: %.2f ms\n", p95);
        printf("  P99: %.2f ms\n", p99);

        double total_s = std::accumulate(encode_times_ms.begin(),
                                          encode_times_ms.end(), 0.0) / 1000.0;
        printf("  Effective FPS: %.1f\n", encode_times_ms.size() / total_s);
    }

    printf("\nEncoded: %u packets, %.2f MB (%u keyframes)\n",
           packet_count, total_encoded_bytes / (1024.0 * 1024.0), keyframe_count);

    if (total_encoded_bytes > 0) {
        printf("\nBitstream written to output.h264\n");
        printf("Verify: ffprobe -v error -show_entries "
               "stream=codec_name,width,height,r_frame_rate "
               "-of default=noprint_wrappers=1 output.h264\n");
        printf("Play:   ffplay output.h264\n");
    }

    MFShutdown();
    CoUninitialize();

    return (encode_times_ms.empty() || packet_count == 0) ? 1 : 0;
}
