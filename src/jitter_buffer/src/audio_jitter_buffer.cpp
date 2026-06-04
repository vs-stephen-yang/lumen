#include "lumen/jitter_buffer/audio_jitter_buffer.h"

namespace lumen {

AudioJitterBuffer::AudioJitterBuffer(Config config)
    : config_(config),
      target_delay_us_(static_cast<uint64_t>(config.target_delay_ms) * 1000) {}

void AudioJitterBuffer::Push(const uint8_t* data, size_t size,
                             const MediaPacketHeader& hdr, uint64_t now_us) {
    stats_.pushed++;

    if (initialized_ && hdr.frame_index < next_expected_) {
        stats_.dropped_late++;
        return;
    }
    if (buffer_.find(hdr.frame_index) != buffer_.end()) {
        stats_.duplicates++;
        return;
    }

    Entry e;
    e.data.assign(data, data + size);
    e.timestamp_us = hdr.timestamp_us;
    e.arrival_us = now_us;
    buffer_.emplace(hdr.frame_index, std::move(e));
}

bool AudioJitterBuffer::PopReady(uint64_t now_us, JitterFrame& out) {
    if (buffer_.empty()) return false;

    if (!initialized_) {
        next_expected_ = buffer_.begin()->first;
        initialized_ = true;
    }

    auto head = buffer_.begin();
    const uint64_t head_index = head->first;
    const Entry& e = head->second;

    if (now_us - e.arrival_us < target_delay_us_) return false;

    if (head_index == next_expected_) {
        out.data = e.data;
        out.frame_index = head_index;
        out.timestamp_us = e.timestamp_us;
        out.is_keyframe = false;
        out.is_concealment = false;
        next_expected_ = head_index + 1;
        buffer_.erase(head);
        stats_.emitted++;
        return true;
    }

    // head_index > next_expected_: the expected slot is missing and its
    // deadline (shared with the buffered head) has passed — conceal it. One
    // PLC marker per missing slot; the head stays until we catch up to it.
    out.data.clear();
    out.frame_index = next_expected_;
    out.timestamp_us = 0;
    out.is_keyframe = false;
    out.is_concealment = true;
    next_expected_++;
    stats_.skipped++;
    return true;
}

}  // namespace lumen
