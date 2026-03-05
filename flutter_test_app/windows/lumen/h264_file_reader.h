#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lumen {

struct NalUnit {
    size_t offset;           // Byte offset of the start code in the bitstream.
    size_t length;           // Total length including start code.
    uint8_t start_code_size; // 3 or 4.
};

/// Parsed H.264 bitstream: raw data + access unit boundaries.
struct H264Bitstream {
    std::vector<uint8_t> data;
    std::vector<NalUnit> nals;
    /// Access unit ranges as [nal_start_index, nal_end_index) pairs.
    std::vector<std::pair<size_t, size_t>> access_units;
};

/// Find all NAL unit boundaries in a raw Annex-B bitstream.
std::vector<NalUnit> FindNalUnits(const uint8_t* data, size_t size);

/// Group NAL units into access units (one frame per AU).
std::vector<std::pair<size_t, size_t>> GroupAccessUnits(
    const uint8_t* data, const std::vector<NalUnit>& nals);

/// Load and parse an H.264 Annex-B bitstream file.
/// Returns empty data vector on file read failure.
H264Bitstream LoadH264File(const std::string& path);

}  // namespace lumen
