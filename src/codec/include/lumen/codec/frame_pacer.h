#pragma once

#include <cstdint>

namespace lumen {

/// Decision returned by the FramePacer for each capture event.
enum class PaceDecision {
    kEncode,     // Normal: encode this frame
    kDrop,       // Frame arrived too early — skip it
    kDuplicate,  // No capture arrived in time — re-encode previous frame
};

/// Platform-agnostic component that decides whether to encode, drop, or
/// duplicate each frame based on the target frame rate.
///
/// The pacer maintains a target interval grid and generates synthetic
/// timestamps that advance at a constant rate, keeping CBR encoders stable
/// regardless of capture jitter.
///
/// Algorithm:
///   - Drop frames arriving < 75% of target interval since last encode.
///   - Duplicate if > 150% of target interval has elapsed (idle capture).
///   - Always advance synthetic timestamps at the target rate.
class FramePacer {
public:
    /// Set the target frame rate. Can be changed at any time.
    void SetTargetFps(uint32_t fps) {
        if (fps == 0) fps = 1;
        target_interval_us_ = 1'000'000LL / fps;
    }

    /// Call when a captured frame arrives.
    /// Returns kEncode if the frame should be encoded, kDrop if it should
    /// be skipped (too close to previous encode).
    PaceDecision ShouldEncode(int64_t capture_time_us) {
        if (last_encode_time_us_ < 0) {
            // First frame — always encode.
            last_encode_time_us_ = capture_time_us;
            return PaceDecision::kEncode;
        }

        int64_t elapsed = capture_time_us - last_encode_time_us_;
        int64_t drop_threshold = target_interval_us_ * 3 / 4;  // 75%

        if (elapsed < drop_threshold) {
            return PaceDecision::kDrop;
        }

        last_encode_time_us_ = capture_time_us;
        return PaceDecision::kEncode;
    }

    /// Call periodically (e.g., on a timer) to check if a frame should be
    /// duplicated because capture has gone idle.
    /// Returns kDuplicate if > 150% of the target interval has elapsed
    /// since the last encode, kEncode otherwise (no action needed).
    PaceDecision Tick(int64_t now_us) {
        if (last_encode_time_us_ < 0) {
            return PaceDecision::kEncode;  // No frames yet — nothing to dup.
        }

        int64_t elapsed = now_us - last_encode_time_us_;
        int64_t dup_threshold = target_interval_us_ * 3 / 2;  // 150%

        if (elapsed >= dup_threshold) {
            last_encode_time_us_ = now_us;
            return PaceDecision::kDuplicate;
        }

        return PaceDecision::kEncode;
    }

    /// Get the synthetic sample timestamp for the MFT (100ns units).
    /// Advances by one frame duration each call.
    int64_t GetSampleTimestamp100ns() {
        int64_t ts = synthetic_timestamp_100ns_;
        synthetic_timestamp_100ns_ += GetSampleDuration100ns();
        return ts;
    }

    /// Get the sample duration in 100ns units based on target FPS.
    int64_t GetSampleDuration100ns() const {
        return target_interval_us_ * 10;  // µs → 100ns
    }

    /// Reset all state (e.g., after a reconfigure).
    void Reset() {
        last_encode_time_us_ = -1;
        // Preserve synthetic_timestamp_100ns_ for timestamp continuity.
    }

    /// Full reset including synthetic timestamps.
    void ResetAll() {
        last_encode_time_us_ = -1;
        synthetic_timestamp_100ns_ = 0;
    }

private:
    int64_t target_interval_us_ = 16'667;    // Default: ~60fps
    int64_t last_encode_time_us_ = -1;
    int64_t synthetic_timestamp_100ns_ = 0;
};

}  // namespace lumen
