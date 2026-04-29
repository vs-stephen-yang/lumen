#pragma once

// Minimal QUIC variable-length integer (RFC 9000 §16) encoder/decoder.
// WebTransport uses these for session IDs and stream-type prefixes:
//   - Bidi WT stream: [varint(0x41)][varint(session_id)][app data]
//   - Uni  WT stream: [varint(0x54)][varint(session_id)][app data]
//   - WT  datagram:   [varint(session_id)][app data]

#include <cstddef>
#include <cstdint>

namespace lumen {

constexpr uint64_t kWtBidiStreamFrame = 0x41;  // WEBTRANSPORT_STREAM frame type
constexpr uint64_t kWtUniStreamType   = 0x54;  // WT unidirectional stream type

/// Encode `value` as a QUIC varint into `out` (max 8 bytes).
/// Returns the number of bytes written, or 0 if `out_size` is too small or
/// `value` exceeds the varint range (2^62 - 1).
size_t VarintEncode(uint64_t value, uint8_t* out, size_t out_size);

/// Decode a varint from `in`. On success, sets `*value` and returns the
/// number of bytes consumed. On failure (insufficient bytes) returns 0.
size_t VarintDecode(const uint8_t* in, size_t in_size, uint64_t* value);

/// Number of bytes required to encode `value`.
inline size_t VarintLen(uint64_t value) {
    if (value < (1ULL <<  6)) return 1;
    if (value < (1ULL << 14)) return 2;
    if (value < (1ULL << 30)) return 4;
    return 8;
}

}  // namespace lumen
