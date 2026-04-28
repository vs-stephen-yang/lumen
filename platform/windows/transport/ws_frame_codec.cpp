#include "ws_frame_codec.h"

#include <cstring>

namespace lumen {

namespace {

constexpr uint8_t kFinBit = 0x80;
constexpr uint8_t kRsvBits = 0x70;
constexpr uint8_t kOpcodeMask = 0x0F;
constexpr uint8_t kMaskBit = 0x80;
constexpr uint8_t kLenMask = 0x7F;

bool IsControlOpcode(uint8_t op) { return (op & 0x8) != 0; }

}  // namespace

WsParseStatus ParseFrame(const uint8_t* data, size_t size, WsFrame& out) {
    if (size < 2) return WsParseStatus::kNeedMoreData;

    const uint8_t b0 = data[0];
    const uint8_t b1 = data[1];

    out.fin = (b0 & kFinBit) != 0;
    if ((b0 & kRsvBits) != 0) return WsParseStatus::kReservedBits;
    if (!out.fin) return WsParseStatus::kFragmented;

    const uint8_t op = b0 & kOpcodeMask;
    if (op != static_cast<uint8_t>(WsOpcode::kBinary) &&
        op != static_cast<uint8_t>(WsOpcode::kText)   &&
        op != static_cast<uint8_t>(WsOpcode::kClose)  &&
        op != static_cast<uint8_t>(WsOpcode::kPing)   &&
        op != static_cast<uint8_t>(WsOpcode::kPong)) {
        return WsParseStatus::kProtocolError;
    }
    out.opcode = static_cast<WsOpcode>(op);

    const bool masked = (b1 & kMaskBit) != 0;
    const uint8_t len7 = b1 & kLenMask;

    size_t cursor = 2;
    uint64_t payload_len = 0;
    if (len7 < 126) {
        payload_len = len7;
    } else if (len7 == 126) {
        if (size < cursor + 2) return WsParseStatus::kNeedMoreData;
        payload_len = (static_cast<uint64_t>(data[cursor]) << 8) |
                      static_cast<uint64_t>(data[cursor + 1]);
        cursor += 2;
    } else {  // 127
        if (size < cursor + 8) return WsParseStatus::kNeedMoreData;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | data[cursor + i];
        }
        cursor += 8;
        if ((payload_len >> 63) != 0) return WsParseStatus::kProtocolError;
    }

    // Control frames (close/ping/pong) have payload <=125 and FIN=1.
    if (IsControlOpcode(op) && payload_len > 125) {
        return WsParseStatus::kProtocolError;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (size < cursor + 4) return WsParseStatus::kNeedMoreData;
        std::memcpy(mask_key, data + cursor, 4);
        cursor += 4;
    }

    if (size < cursor + payload_len) return WsParseStatus::kNeedMoreData;

    out.payload.resize(static_cast<size_t>(payload_len));
    if (payload_len > 0) {
        std::memcpy(out.payload.data(), data + cursor,
                    static_cast<size_t>(payload_len));
        if (masked) {
            for (size_t i = 0; i < out.payload.size(); ++i) {
                out.payload[i] ^= mask_key[i & 3];
            }
        }
    }

    out.consumed_bytes = cursor + static_cast<size_t>(payload_len);
    return WsParseStatus::kOk;
}

std::vector<uint8_t> WriteFrame(WsOpcode opcode,
                                 const uint8_t* payload, size_t payload_size,
                                 bool mask, uint32_t mask_key_be) {
    std::vector<uint8_t> out;
    // Worst-case header is 14 bytes (2 + 8 length + 4 mask).
    out.reserve(14 + payload_size);

    out.push_back(static_cast<uint8_t>(kFinBit |
                                       (static_cast<uint8_t>(opcode) & kOpcodeMask)));

    uint8_t len_byte = mask ? kMaskBit : 0;
    if (payload_size < 126) {
        len_byte |= static_cast<uint8_t>(payload_size);
        out.push_back(len_byte);
    } else if (payload_size <= 0xFFFF) {
        len_byte |= 126;
        out.push_back(len_byte);
        out.push_back(static_cast<uint8_t>((payload_size >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(payload_size & 0xFF));
    } else {
        len_byte |= 127;
        out.push_back(len_byte);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>(
                (static_cast<uint64_t>(payload_size) >> (i * 8)) & 0xFF));
        }
    }

    uint8_t mask_key[4] = {0};
    if (mask) {
        mask_key[0] = static_cast<uint8_t>((mask_key_be >> 24) & 0xFF);
        mask_key[1] = static_cast<uint8_t>((mask_key_be >> 16) & 0xFF);
        mask_key[2] = static_cast<uint8_t>((mask_key_be >> 8) & 0xFF);
        mask_key[3] = static_cast<uint8_t>(mask_key_be & 0xFF);
        out.insert(out.end(), mask_key, mask_key + 4);
    }

    const size_t payload_offset = out.size();
    out.resize(payload_offset + payload_size);
    if (payload_size > 0) {
        std::memcpy(out.data() + payload_offset, payload, payload_size);
        if (mask) {
            for (size_t i = 0; i < payload_size; ++i) {
                out[payload_offset + i] ^= mask_key[i & 3];
            }
        }
    }
    return out;
}

}  // namespace lumen
