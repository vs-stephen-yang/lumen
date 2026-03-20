#pragma once

#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <vector>

namespace lumen {

/// A single fragment: serialized header + payload slice.
struct Fragment {
    std::vector<uint8_t> data;  // [MediaPacketHeader | payload]
};

/// Splits a frame into MTU-sized fragments, each prefixed with a MediaPacketHeader.
class FrameFragmenter {
public:
    /// @param max_payload_size Maximum payload bytes per fragment (excluding header).
    explicit FrameFragmenter(size_t max_payload_size = 1200);

    /// Fragment a frame into packets.
    /// @param frame_data     Raw frame bytes.
    /// @param frame_size     Size of frame_data.
    /// @param header_template Header fields to copy into each fragment
    ///                        (ssrc, frame_index, timestamp_us, flags).
    ///                        fragment_index/fragment_count/is_last_fragment
    ///                        are set automatically.
    /// @return Vector of fragments.
    std::vector<Fragment> Fragment(const uint8_t* frame_data, size_t frame_size,
                                   const MediaPacketHeader& header_template);

    size_t GetMaxPayloadSize() const { return max_payload_size_; }

private:
    size_t max_payload_size_;
};

}  // namespace lumen
