#pragma once

#include <cstdint>
#include <string>

namespace lumen {

/// Per-session config the receiver advertises to the sender via /api/session.
/// The sender uses this to configure WebCodecs and the transport URL.
struct SessionConfig {
    std::string session_id;

    struct Transport {
        std::string kind = "websocket";
        std::string url;          // e.g. "ws://localhost:18443/lumen"
        uint32_t ssrc = 0;
    } transport;

    struct Video {
        std::string codec = "avc1.640028";  // H.264 High@4.0
        uint32_t width = 1920;
        uint32_t height = 1080;
        uint32_t fps = 60;
        uint32_t bitrate = 8'000'000;
    } video;

    struct Audio {
        std::string codec = "opus";
        uint32_t sample_rate = 48000;
        uint32_t channels = 2;
        uint32_t bitrate = 128000;
    } audio;
};

}  // namespace lumen
