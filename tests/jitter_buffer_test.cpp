// Cross-platform unit tests for the video/audio jitter buffers: in-order
// release after the jitter delay, reordering, late-drop, duplicate handling,
// video gap-skip, and audio packet-loss concealment. Clock is injected
// (now_us passed in) so timing is deterministic.

#include "lumen/jitter_buffer/video_jitter_buffer.h"
#include "lumen/jitter_buffer/audio_jitter_buffer.h"
#include "lumen/transport/transport_types.h"

#include <cstdio>
#include <vector>

using namespace lumen;

namespace {
int passed = 0;
int failed = 0;
void check(bool cond, const char* msg) {
    if (cond) ++passed;
    else { std::fprintf(stderr, "FAIL: %s\n", msg); ++failed; }
}

MediaPacketHeader Hdr(uint64_t idx, bool key = false) {
    MediaPacketHeader h;
    h.frame_index = idx;
    h.timestamp_us = idx * 33000;  // ~30fps
    h.SetKeyframe(key);
    return h;
}

// Push a 1-byte payload tagged with the frame index, so we can verify order.
template <typename Buf>
void Push(Buf& b, uint64_t idx, uint64_t now_us, bool key = false) {
    uint8_t byte = static_cast<uint8_t>(idx);
    b.Push(&byte, 1, Hdr(idx, key), now_us);
}
}  // namespace

static void test_video_in_order_after_delay() {
    VideoJitterBuffer jb({/*target_delay_ms=*/50, /*fps=*/30});
    Push(jb, 0, 0, /*key=*/true);
    Push(jb, 1, 0);
    Push(jb, 2, 0);

    JitterFrame f;
    check(!jb.PopReady(10'000, f), "video: not due before delay");

    check(jb.PopReady(50'000, f) && f.frame_index == 0 && f.is_keyframe,
          "video: frame 0 at deadline");
    check(jb.PopReady(50'000, f) && f.frame_index == 1, "video: frame 1");
    check(jb.PopReady(50'000, f) && f.frame_index == 2, "video: frame 2");
    check(!jb.PopReady(50'000, f), "video: empty after draining");
    check(jb.Stats().emitted == 3, "video: emitted 3");
}

static void test_video_reorder() {
    VideoJitterBuffer jb({50, 30});
    Push(jb, 2, 0);
    Push(jb, 0, 0);
    Push(jb, 1, 0);

    JitterFrame f;
    std::vector<uint64_t> order;
    while (jb.PopReady(60'000, f)) order.push_back(f.frame_index);
    check(order.size() == 3 && order[0] == 0 && order[1] == 1 && order[2] == 2,
          "video: reordered to 0,1,2");
}

static void test_video_late_drop() {
    VideoJitterBuffer jb({50, 30});
    Push(jb, 0, 0);
    Push(jb, 1, 0);
    JitterFrame f;
    jb.PopReady(60'000, f);  // emit 0
    jb.PopReady(60'000, f);  // emit 1  -> next_expected = 2
    Push(jb, 1, 60'000);     // late
    check(jb.Stats().dropped_late == 1, "video: late frame dropped");
    check(!jb.PopReady(60'000, f), "video: nothing emitted for late frame");
}

static void test_video_duplicate() {
    VideoJitterBuffer jb({50, 30});
    Push(jb, 0, 0);
    Push(jb, 0, 0);
    check(jb.Stats().duplicates == 1, "video: duplicate counted");
}

static void test_video_gap_skip() {
    VideoJitterBuffer jb({50, 30});
    Push(jb, 0, 0, true);
    Push(jb, 2, 0);  // frame 1 missing

    JitterFrame f;
    check(jb.PopReady(50'000, f) && f.frame_index == 0, "video: emit 0");
    // Now expecting 1, but only 2 is buffered (arrived at t=0). Before its
    // deadline, hold; after, skip the gap and emit 2.
    check(!jb.PopReady(10'000, f), "video: wait for missing 1");
    check(jb.PopReady(50'000, f) && f.frame_index == 2, "video: skip 1, emit 2");
    check(jb.Stats().skipped == 1, "video: one frame skipped");
}

static void test_audio_in_order_and_conceal() {
    AudioJitterBuffer jb({/*target_delay_ms=*/40});
    Push(jb, 0, 0);
    Push(jb, 2, 0);  // frame 1 lost

    JitterFrame f;
    check(jb.PopReady(40'000, f) && f.frame_index == 0 && !f.is_concealment,
          "audio: emit 0");
    // Expected 1 is missing -> concealment marker for slot 1.
    check(jb.PopReady(40'000, f) && f.frame_index == 1 && f.is_concealment &&
              f.data.empty(),
          "audio: conceal slot 1");
    // Then the real frame 2.
    check(jb.PopReady(40'000, f) && f.frame_index == 2 && !f.is_concealment,
          "audio: emit 2");
    check(jb.Stats().emitted == 2 && jb.Stats().skipped == 1,
          "audio: 2 emitted, 1 concealed");
}

int main() {
    test_video_in_order_after_delay();
    test_video_reorder();
    test_video_late_drop();
    test_video_duplicate();
    test_video_gap_skip();
    test_audio_in_order_and_conceal();
    std::printf("jitter_buffer_test: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
