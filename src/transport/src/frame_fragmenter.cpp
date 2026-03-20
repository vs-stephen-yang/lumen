#include "frame_fragmenter.h"

#include <algorithm>
#include <cstring>

namespace lumen {

FrameFragmenter::FrameFragmenter(size_t max_payload_size)
    : max_payload_size_(max_payload_size) {}

std::vector<lumen::Fragment> FrameFragmenter::Fragment(
    const uint8_t* frame_data, size_t frame_size,
    const MediaPacketHeader& header_template) {
    if (frame_size == 0) return {};

    const size_t fragment_count =
        (frame_size + max_payload_size_ - 1) / max_payload_size_;

    std::vector<lumen::Fragment> fragments;
    fragments.reserve(fragment_count);

    size_t offset = 0;
    for (size_t i = 0; i < fragment_count; ++i) {
        const size_t payload_size =
            std::min(max_payload_size_, frame_size - offset);

        MediaPacketHeader hdr = header_template;
        hdr.fragment_index = static_cast<uint16_t>(i);
        hdr.fragment_count = static_cast<uint16_t>(fragment_count);
        hdr.SetLastFragment(i == fragment_count - 1);

        lumen::Fragment frag;
        frag.data.resize(MediaPacketHeader::kSerializedSize + payload_size);
        hdr.Serialize(frag.data.data());
        std::memcpy(frag.data.data() + MediaPacketHeader::kSerializedSize,
                     frame_data + offset, payload_size);

        fragments.push_back(std::move(frag));
        offset += payload_size;
    }

    return fragments;
}

}  // namespace lumen
