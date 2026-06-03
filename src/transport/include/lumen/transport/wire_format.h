#pragma once

#include "lumen/transport/transport_types.h"

#include <cstdint>

namespace lumen::wire {

// Canonical on-the-wire channel identifiers.
//
// Every multiplexed transport message carries a leading channel_id byte that
// selects the logical channel. These constants are the single source of truth
// for that byte. They are PINNED — do not derive them from ChannelType's enum
// order, otherwise reordering the enum would silently change the wire format.
// The browser sender mirrors these values (see
// platform/windows/apps/web_receiver/test_page/index.html: CHAN_*).
//
// Per-transport framing (channel_id is always the first byte):
//   TCP:           [channel_id:u8][length:u32][MediaPacketHeader:28][payload]
//   WebSocket:     [channel_id:u8][MediaPacketHeader:28][payload]   (one WS msg)
//   WebTransport:  [channel_id:u8][MediaPacketHeader:28][fragment]  (one datagram)
inline constexpr uint8_t kChannelVideo   = 0;
inline constexpr uint8_t kChannelAudio   = 1;
inline constexpr uint8_t kChannelControl = 2;

/// Map a logical channel to its wire channel_id byte.
inline uint8_t ChannelIdOf(ChannelType type) {
    switch (type) {
        case ChannelType::kVideo:   return kChannelVideo;
        case ChannelType::kAudio:   return kChannelAudio;
        case ChannelType::kControl: return kChannelControl;
    }
    return kChannelVideo;
}

/// Map a wire channel_id byte back to a logical channel.
/// Returns false (and leaves `out` untouched) for an unknown id.
inline bool ChannelTypeOf(uint8_t id, ChannelType& out) {
    switch (id) {
        case kChannelVideo:   out = ChannelType::kVideo;   return true;
        case kChannelAudio:   out = ChannelType::kAudio;   return true;
        case kChannelControl: out = ChannelType::kControl; return true;
        default:              return false;
    }
}

}  // namespace lumen::wire
