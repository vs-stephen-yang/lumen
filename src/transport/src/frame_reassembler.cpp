#include "frame_reassembler.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace lumen {

FrameReassembler::FrameReassembler(uint64_t timeout_us)
    : timeout_us_(timeout_us) {}

uint64_t FrameReassembler::MakeKey(uint32_t ssrc, uint64_t frame_index) {
    return (static_cast<uint64_t>(ssrc) << 32) ^ frame_index;
}

void FrameReassembler::AddFragment(const uint8_t* packet_data,
                                    size_t packet_size) {
    if (packet_size < MediaPacketHeader::kSerializedSize) return;

    MediaPacketHeader hdr;
    if (!MediaPacketHeader::Deserialize(packet_data, packet_size, hdr)) return;
    if (hdr.fragment_count == 0) return;
    if (hdr.fragment_index >= hdr.fragment_count) return;

    const uint8_t* payload = packet_data + MediaPacketHeader::kSerializedSize;
    const size_t payload_size = packet_size - MediaPacketHeader::kSerializedSize;

    const uint64_t key = MakeKey(hdr.ssrc, hdr.frame_index);

    auto& pending = pending_[key];
    if (pending.fragment_count == 0) {
        // First fragment for this frame.
        pending.fragment_count = hdr.fragment_count;
        pending.fragments.resize(hdr.fragment_count);
        pending.first_header = hdr;
        auto now = std::chrono::steady_clock::now();
        pending.first_arrival_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count());
    }

    // Ignore duplicate fragments.
    if (!pending.fragments[hdr.fragment_index].empty()) return;

    pending.fragments[hdr.fragment_index].assign(payload,
                                                  payload + payload_size);
    pending.received_count++;

    if (pending.received_count == pending.fragment_count) {
        // Frame complete — reassemble.
        size_t total_size = 0;
        for (auto& frag : pending.fragments) total_size += frag.size();

        std::vector<uint8_t> frame_data;
        frame_data.reserve(total_size);
        for (auto& frag : pending.fragments) {
            frame_data.insert(frame_data.end(), frag.begin(), frag.end());
        }

        MediaPacketHeader frame_header = pending.first_header;
        pending_.erase(key);

        if (callback_) {
            callback_(frame_data.data(), frame_data.size(), frame_header);
        }
    }
}

void FrameReassembler::PurgeStale(uint64_t now_us) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_us - it->second.first_arrival_us > timeout_us_) {
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace lumen
