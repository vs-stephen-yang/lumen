/// Cross-platform unit tests for transport layer primitives:
/// - MediaPacketHeader serialization/deserialization
/// - FrameFragmenter
/// - FrameReassembler

#include "lumen/transport/transport_types.h"
#include "frame_fragmenter.h"
#include "frame_reassembler.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace lumen;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                                        \
    static void test_##name();                            \
    struct TestRunner_##name {                             \
        TestRunner_##name() {                              \
            printf("  %-50s ", #name);                    \
            try {                                          \
                test_##name();                             \
                printf("PASS\n");                         \
                tests_passed++;                            \
            } catch (...) {                               \
                printf("FAIL\n");                         \
                tests_failed++;                            \
            }                                              \
        }                                                  \
    } test_runner_##name;                                  \
    static void test_##name()

#define ASSERT_TRUE(expr)                                  \
    do {                                                   \
        if (!(expr)) {                                     \
            printf("ASSERT_TRUE failed: %s\n", #expr);    \
            throw std::runtime_error("assertion failed");  \
        }                                                  \
    } while (0)

#define ASSERT_EQ(a, b)                                    \
    do {                                                   \
        if ((a) != (b)) {                                  \
            printf("ASSERT_EQ failed: %s != %s\n",        \
                   #a, #b);                                \
            throw std::runtime_error("assertion failed");  \
        }                                                  \
    } while (0)

// ==================== MediaPacketHeader Tests ====================

TEST(header_serialize_deserialize_roundtrip) {
    MediaPacketHeader hdr;
    hdr.ssrc = 0x12345678;
    hdr.frame_index = 0xDEADBEEFCAFEBABE;
    hdr.timestamp_us = 1234567890123456ULL;
    hdr.fragment_index = 42;
    hdr.fragment_count = 100;
    hdr.SetKeyframe(true);
    hdr.SetLastFragment(false);

    uint8_t buf[MediaPacketHeader::kSerializedSize];
    hdr.Serialize(buf);

    MediaPacketHeader out;
    ASSERT_TRUE(MediaPacketHeader::Deserialize(buf, sizeof(buf), out));

    ASSERT_EQ(out.ssrc, 0x12345678u);
    ASSERT_EQ(out.frame_index, 0xDEADBEEFCAFEBABEull);
    ASSERT_EQ(out.timestamp_us, 1234567890123456ULL);
    ASSERT_EQ(out.fragment_index, 42u);
    ASSERT_EQ(out.fragment_count, 100u);
    ASSERT_TRUE(out.IsKeyframe());
    ASSERT_TRUE(!out.IsLastFragment());
}

TEST(header_flags_keyframe_and_last_fragment) {
    MediaPacketHeader hdr;
    hdr.SetKeyframe(true);
    hdr.SetLastFragment(true);

    uint8_t buf[MediaPacketHeader::kSerializedSize];
    hdr.Serialize(buf);

    MediaPacketHeader out;
    ASSERT_TRUE(MediaPacketHeader::Deserialize(buf, sizeof(buf), out));
    ASSERT_TRUE(out.IsKeyframe());
    ASSERT_TRUE(out.IsLastFragment());

    // Clear keyframe, keep last fragment.
    hdr.SetKeyframe(false);
    hdr.Serialize(buf);
    ASSERT_TRUE(MediaPacketHeader::Deserialize(buf, sizeof(buf), out));
    ASSERT_TRUE(!out.IsKeyframe());
    ASSERT_TRUE(out.IsLastFragment());
}

TEST(header_version_field) {
    MediaPacketHeader hdr;
    ASSERT_EQ(hdr.Version(), 0u);  // default is current version (0)

    hdr.SetVersion(5);
    hdr.SetKeyframe(true);
    hdr.SetLastFragment(true);

    uint8_t buf[MediaPacketHeader::kSerializedSize];
    hdr.Serialize(buf);

    MediaPacketHeader out;
    ASSERT_TRUE(MediaPacketHeader::Deserialize(buf, sizeof(buf), out));
    ASSERT_EQ(out.Version(), 5u);
    // Version bits are independent of the keyframe / last-fragment bits.
    ASSERT_TRUE(out.IsKeyframe());
    ASSERT_TRUE(out.IsLastFragment());

    // Clearing the version leaves the low flag bits intact.
    out.SetVersion(0);
    ASSERT_EQ(out.Version(), 0u);
    ASSERT_TRUE(out.IsKeyframe());
    ASSERT_TRUE(out.IsLastFragment());
}

TEST(header_deserialize_too_short) {
    uint8_t buf[10] = {};
    MediaPacketHeader out;
    ASSERT_TRUE(!MediaPacketHeader::Deserialize(buf, sizeof(buf), out));
}

TEST(header_zero_values) {
    MediaPacketHeader hdr;  // All zeros.
    uint8_t buf[MediaPacketHeader::kSerializedSize];
    hdr.Serialize(buf);

    MediaPacketHeader out;
    ASSERT_TRUE(MediaPacketHeader::Deserialize(buf, sizeof(buf), out));
    ASSERT_EQ(out.ssrc, 0u);
    ASSERT_EQ(out.frame_index, 0ull);
    ASSERT_EQ(out.timestamp_us, 0ull);
    ASSERT_EQ(out.fragment_index, 0u);
    ASSERT_EQ(out.fragment_count, 0u);
    ASSERT_EQ(out.flags, 0u);
}

// ==================== FrameFragmenter Tests ====================

TEST(fragmenter_single_fragment) {
    FrameFragmenter frag(1200);
    std::vector<uint8_t> frame(500, 0xAB);

    MediaPacketHeader hdr;
    hdr.ssrc = 1;
    hdr.frame_index = 10;
    hdr.timestamp_us = 999;
    hdr.SetKeyframe(true);

    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);

    ASSERT_EQ(fragments.size(), 1u);
    ASSERT_EQ(fragments[0].data.size(),
              MediaPacketHeader::kSerializedSize + 500);

    // Verify header.
    MediaPacketHeader out;
    ASSERT_TRUE(MediaPacketHeader::Deserialize(
        fragments[0].data.data(), fragments[0].data.size(), out));
    ASSERT_EQ(out.fragment_index, 0u);
    ASSERT_EQ(out.fragment_count, 1u);
    ASSERT_TRUE(out.IsKeyframe());
    ASSERT_TRUE(out.IsLastFragment());
}

TEST(fragmenter_5000_byte_frame) {
    const size_t max_payload = 1200;
    FrameFragmenter frag(max_payload);
    std::vector<uint8_t> frame(5000);
    std::iota(frame.begin(), frame.end(), static_cast<uint8_t>(0));

    MediaPacketHeader hdr;
    hdr.ssrc = 2;
    hdr.frame_index = 42;

    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);

    // 5000 / 1200 = 4.17 -> 5 fragments
    ASSERT_EQ(fragments.size(), 5u);

    // Verify fragment indices and last-fragment flag.
    for (size_t i = 0; i < fragments.size(); ++i) {
        MediaPacketHeader out;
        ASSERT_TRUE(MediaPacketHeader::Deserialize(
            fragments[i].data.data(), fragments[i].data.size(), out));
        ASSERT_EQ(out.fragment_index, static_cast<uint16_t>(i));
        ASSERT_EQ(out.fragment_count, 5u);
        ASSERT_EQ(out.ssrc, 2u);
        ASSERT_EQ(out.frame_index, 42ull);

        if (i == 4) {
            ASSERT_TRUE(out.IsLastFragment());
        } else {
            ASSERT_TRUE(!out.IsLastFragment());
        }
    }

    // Verify payload data integrity.
    std::vector<uint8_t> reassembled;
    for (auto& f : fragments) {
        reassembled.insert(reassembled.end(),
                           f.data.begin() + MediaPacketHeader::kSerializedSize,
                           f.data.end());
    }
    ASSERT_EQ(reassembled.size(), frame.size());
    ASSERT_TRUE(reassembled == frame);
}

TEST(fragmenter_empty_frame) {
    FrameFragmenter frag(1200);
    MediaPacketHeader hdr;
    auto fragments = frag.Fragment(nullptr, 0, hdr);
    ASSERT_EQ(fragments.size(), 0u);
}

TEST(fragmenter_exact_mtu) {
    const size_t max_payload = 1200;
    FrameFragmenter frag(max_payload);
    std::vector<uint8_t> frame(1200, 0xFF);

    MediaPacketHeader hdr;
    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);
    ASSERT_EQ(fragments.size(), 1u);
}

// ==================== FrameReassembler Tests ====================

TEST(reassembler_in_order) {
    FrameReassembler reasm(500'000);

    bool received = false;
    std::vector<uint8_t> received_data;

    reasm.SetCallback(
        [&](const uint8_t* data, size_t size, const MediaPacketHeader& hdr) {
            received = true;
            received_data.assign(data, data + size);
        });

    // Create a 3-fragment frame.
    const size_t max_payload = 100;
    FrameFragmenter frag(max_payload);
    std::vector<uint8_t> frame(250);
    std::iota(frame.begin(), frame.end(), static_cast<uint8_t>(0));

    MediaPacketHeader hdr;
    hdr.ssrc = 1;
    hdr.frame_index = 1;

    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);
    ASSERT_EQ(fragments.size(), 3u);

    // Feed in order.
    for (auto& f : fragments) {
        reasm.AddFragment(f.data.data(), f.data.size());
    }

    ASSERT_TRUE(received);
    ASSERT_EQ(received_data.size(), frame.size());
    ASSERT_TRUE(received_data == frame);
}

TEST(reassembler_out_of_order) {
    FrameReassembler reasm(500'000);

    bool received = false;
    std::vector<uint8_t> received_data;

    reasm.SetCallback(
        [&](const uint8_t* data, size_t size, const MediaPacketHeader&) {
            received = true;
            received_data.assign(data, data + size);
        });

    const size_t max_payload = 100;
    FrameFragmenter frag(max_payload);
    std::vector<uint8_t> frame(250);
    std::iota(frame.begin(), frame.end(), static_cast<uint8_t>(0));

    MediaPacketHeader hdr;
    hdr.ssrc = 1;
    hdr.frame_index = 2;

    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);
    ASSERT_EQ(fragments.size(), 3u);

    // Feed in reverse order.
    reasm.AddFragment(fragments[2].data.data(), fragments[2].data.size());
    ASSERT_TRUE(!received);

    reasm.AddFragment(fragments[0].data.data(), fragments[0].data.size());
    ASSERT_TRUE(!received);

    reasm.AddFragment(fragments[1].data.data(), fragments[1].data.size());
    ASSERT_TRUE(received);
    ASSERT_EQ(received_data.size(), frame.size());
    ASSERT_TRUE(received_data == frame);
}

TEST(reassembler_duplicate_fragment) {
    FrameReassembler reasm(500'000);

    int callback_count = 0;

    reasm.SetCallback(
        [&](const uint8_t*, size_t, const MediaPacketHeader&) {
            callback_count++;
        });

    const size_t max_payload = 100;
    FrameFragmenter frag(max_payload);
    std::vector<uint8_t> frame(150);

    MediaPacketHeader hdr;
    hdr.ssrc = 1;
    hdr.frame_index = 3;

    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);
    ASSERT_EQ(fragments.size(), 2u);

    // Send first fragment twice, then second.
    reasm.AddFragment(fragments[0].data.data(), fragments[0].data.size());
    reasm.AddFragment(fragments[0].data.data(), fragments[0].data.size());
    reasm.AddFragment(fragments[1].data.data(), fragments[1].data.size());

    ASSERT_EQ(callback_count, 1);
}

TEST(reassembler_stale_purge) {
    FrameReassembler reasm(1000);  // 1ms timeout for testing

    int callback_count = 0;
    reasm.SetCallback(
        [&](const uint8_t*, size_t, const MediaPacketHeader&) {
            callback_count++;
        });

    const size_t max_payload = 100;
    FrameFragmenter frag(max_payload);
    std::vector<uint8_t> frame(250);

    MediaPacketHeader hdr;
    hdr.ssrc = 1;
    hdr.frame_index = 4;

    auto fragments = frag.Fragment(frame.data(), frame.size(), hdr);

    // Only send first fragment (incomplete frame).
    reasm.AddFragment(fragments[0].data.data(), fragments[0].data.size());

    // Purge with a timestamp far in the future.
    auto now = std::chrono::steady_clock::now();
    uint64_t future_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count()) +
        1'000'000;  // 1 second in the future
    reasm.PurgeStale(future_us);

    // Now send remaining fragments — should NOT trigger callback (purged).
    reasm.AddFragment(fragments[1].data.data(), fragments[1].data.size());
    reasm.AddFragment(fragments[2].data.data(), fragments[2].data.size());

    ASSERT_EQ(callback_count, 0);
}

int main() {
    printf("=== Transport Unit Tests ===\n");
    // Tests run automatically via static initialization.
    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
