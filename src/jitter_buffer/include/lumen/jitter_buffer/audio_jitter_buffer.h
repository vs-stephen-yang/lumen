#pragma once

#include "lumen/jitter_buffer/jitter_buffer.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <map>
#include <vector>

namespace lumen {

/// Reorders incoming audio packets by frame_index and releases them in order
/// after a small buffering delay. Unlike video, a missing slot is not silently
/// skipped: once its deadline passes the buffer emits a concealment marker
/// (is_concealment = true) so the caller runs packet-loss concealment
/// (AudioDecoder::DecodePLC) for that slot, keeping the audio clock continuous.
///
/// Clock-injected and pull-based for deterministic testing.
class AudioJitterBuffer {
public:
    struct Config {
        uint32_t target_delay_ms = 40;
    };

    explicit AudioJitterBuffer(Config config = {});

    void Push(const uint8_t* data, size_t size, const MediaPacketHeader& hdr,
              uint64_t now_us);

    /// Release the next slot if due: a real packet when present, otherwise a
    /// concealment marker once the gap deadline has passed.
    bool PopReady(uint64_t now_us, JitterFrame& out);

    JitterStats Stats() const { return stats_; }
    size_t Size() const { return buffer_.size(); }

private:
    struct Entry {
        std::vector<uint8_t> data;
        uint64_t timestamp_us = 0;
        uint64_t arrival_us = 0;
    };

    Config config_;
    uint64_t target_delay_us_;
    bool initialized_ = false;  // next_expected_ anchored at first PopReady
    uint64_t next_expected_ = 0;
    std::map<uint64_t, Entry> buffer_;
    JitterStats stats_;
};

}  // namespace lumen
