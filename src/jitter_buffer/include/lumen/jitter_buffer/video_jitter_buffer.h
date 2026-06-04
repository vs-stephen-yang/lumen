#pragma once

#include "lumen/jitter_buffer/jitter_buffer.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace lumen {

/// Reorders incoming video frames by frame_index and releases them in order
/// after a small buffering delay, absorbing network jitter. Missing frames
/// create a gap; once the gap's deadline passes the buffer skips it (the
/// caller should treat a skip as a freeze/keyframe-request hint) and releases
/// the next available frame.
///
/// Clock-injected and pull-based for deterministic testing: the owner passes
/// `now_us` (a steady microsecond clock) to Push()/PopReady(); PopReady()
/// returns the next frame that is due, or false.
class VideoJitterBuffer {
public:
    struct Config {
        uint32_t target_delay_ms = 50;  // jitter cushion before release
        uint32_t fps = 30;              // reserved for future pacing
    };

    VideoJitterBuffer() : VideoJitterBuffer(Config{}) {}
    explicit VideoJitterBuffer(Config config);

    /// Add a reassembled frame. `data`/`size` is the frame payload (no header).
    void Push(const uint8_t* data, size_t size, const MediaPacketHeader& hdr,
              uint64_t now_us);

    /// Release the next in-order frame if it is due (or a gap deadline has
    /// passed). Returns true and fills `out` when a frame is released.
    bool PopReady(uint64_t now_us, JitterFrame& out);

    JitterStats Stats() const { return stats_; }
    size_t Size() const { return buffer_.size(); }

private:
    struct Entry {
        std::vector<uint8_t> data;
        uint64_t timestamp_us = 0;
        bool is_keyframe = false;
        uint64_t arrival_us = 0;
    };

    Config config_;
    uint64_t target_delay_us_;
    bool initialized_ = false;  // next_expected_ anchored at first PopReady
    uint64_t next_expected_ = 0;
    std::map<uint64_t, Entry> buffer_;  // ordered by frame_index
    JitterStats stats_;
};

}  // namespace lumen
