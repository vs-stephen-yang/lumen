#pragma once

#include <cstdint>
#include <vector>

namespace lumen {

/// A unit emitted by a jitter buffer, in playout order.
struct JitterFrame {
    std::vector<uint8_t> data;       // media payload (header already stripped)
    uint64_t frame_index = 0;
    uint64_t timestamp_us = 0;       // capture timestamp (for A/V sync)
    bool is_keyframe = false;
    /// Audio only: no real packet was available for this index — the caller
    /// should run packet-loss concealment (AudioDecoder::DecodePLC) instead of
    /// decoding `data` (which is empty).
    bool is_concealment = false;
};

/// Runtime counters, useful for tests and telemetry.
struct JitterStats {
    uint64_t pushed = 0;
    uint64_t emitted = 0;
    uint64_t dropped_late = 0;   // arrived after its slot was already emitted
    uint64_t duplicates = 0;     // same frame_index pushed twice
    uint64_t skipped = 0;        // video: gaps skipped; audio: concealed slots
};

}  // namespace lumen
