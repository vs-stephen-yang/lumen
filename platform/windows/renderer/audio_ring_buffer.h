#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lumen {

/// Lock-free single-producer single-consumer ring buffer for interleaved
/// float32 audio samples. Capacity must be a power of two.
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity)
        : mask_(NextPowerOfTwo(capacity) - 1),
          buffer_(NextPowerOfTwo(capacity), 0.0f) {}

    /// Write up to `count` samples. Returns number actually written.
    size_t Write(const float* data, size_t count) {
        size_t writable = WritableCount();
        size_t to_write = (count < writable) ? count : writable;
        if (to_write == 0) return 0;

        size_t wi = write_index_.load(std::memory_order_relaxed);
        size_t pos = wi & mask_;
        size_t capacity = mask_ + 1;

        // May need two memcpy if wrapping around
        size_t first = capacity - pos;
        if (first >= to_write) {
            std::memcpy(&buffer_[pos], data, to_write * sizeof(float));
        } else {
            std::memcpy(&buffer_[pos], data, first * sizeof(float));
            std::memcpy(&buffer_[0], data + first,
                        (to_write - first) * sizeof(float));
        }

        write_index_.store(wi + to_write, std::memory_order_release);
        return to_write;
    }

    /// Read up to `count` samples. Returns number actually read.
    size_t Read(float* data, size_t count) {
        size_t readable = ReadableCount();
        size_t to_read = (count < readable) ? count : readable;
        if (to_read == 0) return 0;

        size_t ri = read_index_.load(std::memory_order_relaxed);
        size_t pos = ri & mask_;
        size_t capacity = mask_ + 1;

        size_t first = capacity - pos;
        if (first >= to_read) {
            std::memcpy(data, &buffer_[pos], to_read * sizeof(float));
        } else {
            std::memcpy(data, &buffer_[pos], first * sizeof(float));
            std::memcpy(data + first, &buffer_[0],
                        (to_read - first) * sizeof(float));
        }

        read_index_.store(ri + to_read, std::memory_order_release);
        return to_read;
    }

    /// Number of samples available to read.
    size_t ReadableCount() const {
        size_t wi = write_index_.load(std::memory_order_acquire);
        size_t ri = read_index_.load(std::memory_order_relaxed);
        return wi - ri;
    }

    /// Number of samples that can be written.
    size_t WritableCount() const {
        size_t capacity = mask_ + 1;
        return capacity - ReadableCount();
    }

    /// Discard all buffered data.
    void Clear() {
        read_index_.store(write_index_.load(std::memory_order_acquire),
                          std::memory_order_release);
    }

    size_t Capacity() const { return mask_ + 1; }

private:
    static size_t NextPowerOfTwo(size_t v) {
        if (v == 0) return 1;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        return v + 1;
    }

    size_t mask_;
    std::vector<float> buffer_;
    // Monotonically increasing indices; masked on access.
    // Separate cache lines to avoid false sharing.
    alignas(64) std::atomic<size_t> write_index_{0};
    alignas(64) std::atomic<size_t> read_index_{0};
};

}  // namespace lumen
