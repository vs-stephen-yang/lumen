#include "lumen/transport/transport_types.h"

#include <cstring>

namespace lumen {

namespace {

void WriteU16(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[1] = static_cast<uint8_t>(val & 0xFF);
}

void WriteU32(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(val & 0xFF);
}

void WriteU64(uint8_t* buf, uint64_t val) {
    for (int i = 7; i >= 0; --i) {
        buf[7 - i] = static_cast<uint8_t>((val >> (i * 8)) & 0xFF);
    }
}

uint16_t ReadU16(const uint8_t* buf) {
    return static_cast<uint16_t>((buf[0] << 8) | buf[1]);
}

uint32_t ReadU32(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) << 24) |
           (static_cast<uint32_t>(buf[1]) << 16) |
           (static_cast<uint32_t>(buf[2]) << 8) |
           static_cast<uint32_t>(buf[3]);
}

uint64_t ReadU64(const uint8_t* buf) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val = (val << 8) | buf[i];
    }
    return val;
}

}  // namespace

void MediaPacketHeader::Serialize(uint8_t* buf) const {
    // Layout (28 bytes, network byte order):
    //  [0..3]   ssrc
    //  [4..11]  frame_index
    //  [12..19] timestamp_us
    //  [20..21] fragment_index
    //  [22..23] fragment_count
    //  [24..27] flags
    WriteU32(buf + 0, ssrc);
    WriteU64(buf + 4, frame_index);
    WriteU64(buf + 12, timestamp_us);
    WriteU16(buf + 20, fragment_index);
    WriteU16(buf + 22, fragment_count);
    WriteU32(buf + 24, flags);
}

bool MediaPacketHeader::Deserialize(const uint8_t* buf, size_t len,
                                     MediaPacketHeader& out) {
    if (len < kSerializedSize) return false;

    out.ssrc = ReadU32(buf + 0);
    out.frame_index = ReadU64(buf + 4);
    out.timestamp_us = ReadU64(buf + 12);
    out.fragment_index = ReadU16(buf + 20);
    out.fragment_count = ReadU16(buf + 22);
    out.flags = ReadU32(buf + 24);
    return true;
}

}  // namespace lumen
