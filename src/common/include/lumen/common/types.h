#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lumen {

/// Pixel formats used throughout the pipeline.
enum class PixelFormat {
    kBGRA,       // B8G8R8A8_UNORM — capture output format
    kNV12,       // Y plane + interleaved UV — encoder input format
    kRGBA,       // R8G8B8A8_UNORM
    kR16G16B16A16Float,  // HDR scRGB
};

/// Video codec identifiers.
enum class VideoCodec {
    kH264,
    kHEVC,
    kAV1,
};

/// Encoder rate control modes.
enum class RateControlMode {
    kCBR,   // Constant bitrate
    kVBR,   // Variable bitrate
    kCQP,   // Constant quantization parameter
};

/// Describes a video frame's dimensions and format.
struct VideoFrameDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat format = PixelFormat::kBGRA;
};

/// Timestamp in microseconds.
using Timestamp = int64_t;

/// Metadata attached to each captured or encoded frame.
struct FrameMetadata {
    uint64_t frame_index = 0;
    Timestamp capture_time_us = 0;    // Time of capture (µs since epoch)
    Timestamp encode_start_us = 0;    // When encoding began
    Timestamp encode_end_us = 0;      // When encoding completed
    bool is_keyframe = false;
};

/// Configuration for video encoding.
struct VideoEncoderConfig {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrate_bps = 8'000'000;
    uint32_t peak_bitrate_bps = 0;            // VBR peak; 0 = 1.5x mean
    uint32_t gop_size_frames = 0;             // 0 = driver default
    VideoCodec codec = VideoCodec::kH264;
    RateControlMode rate_control = RateControlMode::kCBR;
    bool low_latency = true;
    bool intra_refresh = false;
    uint32_t intra_refresh_period_frames = 60;
    uint32_t max_slice_size_bytes = 0;        // 0 = disabled
};

/// An encoded video packet (bitstream data on CPU).
struct EncodedPacket {
    std::vector<uint8_t> data;
    FrameMetadata metadata;
};

}  // namespace lumen
