#pragma once

#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace lumen {

/// Callback fired when a complete frame has been reassembled.
/// Parameters: frame data, frame size, header of the first fragment.
using ReassembledFrameCallback =
    std::function<void(const uint8_t* data, size_t size,
                       const MediaPacketHeader& header)>;

/// Collects fragments and reassembles complete frames.
class FrameReassembler {
public:
    /// @param timeout_us Discard incomplete frames older than this (microseconds).
    explicit FrameReassembler(uint64_t timeout_us = 500'000);

    /// Add a fragment (raw packet: header + payload).
    /// Fires the callback if this completes a frame.
    void AddFragment(const uint8_t* packet_data, size_t packet_size);

    /// Purge incomplete frames with first-fragment arrival older than now - timeout.
    void PurgeStale(uint64_t now_us);

    void SetCallback(ReassembledFrameCallback callback) {
        callback_ = std::move(callback);
    }

private:
    struct PendingFrame {
        uint16_t fragment_count = 0;
        uint16_t received_count = 0;
        uint64_t first_arrival_us = 0;
        MediaPacketHeader first_header;
        std::vector<std::vector<uint8_t>> fragments;  // indexed by fragment_index
    };

    /// Key combining ssrc and frame_index for deduplication.
    static uint64_t MakeKey(uint32_t ssrc, uint64_t frame_index);

    uint64_t timeout_us_;
    std::unordered_map<uint64_t, PendingFrame> pending_;
    ReassembledFrameCallback callback_;
};

}  // namespace lumen
