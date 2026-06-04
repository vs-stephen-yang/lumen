#include "lumen/jitter_buffer/video_jitter_buffer.h"

namespace lumen {

VideoJitterBuffer::VideoJitterBuffer(Config config)
    : config_(config),
      target_delay_us_(static_cast<uint64_t>(config.target_delay_ms) * 1000) {}

void VideoJitterBuffer::Push(const uint8_t* data, size_t size,
                             const MediaPacketHeader& hdr, uint64_t now_us) {
    stats_.pushed++;

    // Arrived after its slot was already released. Only meaningful once we
    // have anchored next_expected_ (at the first PopReady) — before that we
    // buffer everything so out-of-order startup still finds the true minimum.
    if (initialized_ && hdr.frame_index < next_expected_) {
        stats_.dropped_late++;
        return;
    }
    // Already buffered.
    if (buffer_.find(hdr.frame_index) != buffer_.end()) {
        stats_.duplicates++;
        return;
    }

    Entry e;
    e.data.assign(data, data + size);
    e.timestamp_us = hdr.timestamp_us;
    e.is_keyframe = hdr.IsKeyframe();
    e.arrival_us = now_us;
    buffer_.emplace(hdr.frame_index, std::move(e));
}

bool VideoJitterBuffer::PopReady(uint64_t now_us, JitterFrame& out) {
    if (buffer_.empty()) return false;

    // Anchor the expected index to the lowest frame seen, so reordered
    // startup releases from the true beginning rather than the first arrival.
    if (!initialized_) {
        next_expected_ = buffer_.begin()->first;
        initialized_ = true;
    }

    auto head = buffer_.begin();          // smallest buffered frame_index
    const uint64_t head_index = head->first;
    const Entry& e = head->second;

    // Release once the head has waited out the jitter cushion. When the head
    // is past the expected index (a gap), the same deadline decides when to
    // give up waiting for the missing frames and skip them.
    if (now_us - e.arrival_us < target_delay_us_) return false;

    if (head_index > next_expected_) {
        stats_.skipped += (head_index - next_expected_);
    }

    out.data = e.data;
    out.frame_index = head_index;
    out.timestamp_us = e.timestamp_us;
    out.is_keyframe = e.is_keyframe;
    out.is_concealment = false;

    next_expected_ = head_index + 1;
    buffer_.erase(head);
    stats_.emitted++;
    return true;
}

}  // namespace lumen
