#include "wt_varint.h"

namespace lumen {

size_t VarintEncode(uint64_t v, uint8_t* out, size_t out_size) {
    if (v < (1ULL << 6)) {
        if (out_size < 1) return 0;
        out[0] = static_cast<uint8_t>(v);
        return 1;
    }
    if (v < (1ULL << 14)) {
        if (out_size < 2) return 0;
        out[0] = static_cast<uint8_t>(0x40 | ((v >> 8) & 0x3F));
        out[1] = static_cast<uint8_t>(v & 0xFF);
        return 2;
    }
    if (v < (1ULL << 30)) {
        if (out_size < 4) return 0;
        out[0] = static_cast<uint8_t>(0x80 | ((v >> 24) & 0x3F));
        out[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
        out[3] = static_cast<uint8_t>( v        & 0xFF);
        return 4;
    }
    if (v < (1ULL << 62)) {
        if (out_size < 8) return 0;
        out[0] = static_cast<uint8_t>(0xC0 | ((v >> 56) & 0x3F));
        out[1] = static_cast<uint8_t>((v >> 48) & 0xFF);
        out[2] = static_cast<uint8_t>((v >> 40) & 0xFF);
        out[3] = static_cast<uint8_t>((v >> 32) & 0xFF);
        out[4] = static_cast<uint8_t>((v >> 24) & 0xFF);
        out[5] = static_cast<uint8_t>((v >> 16) & 0xFF);
        out[6] = static_cast<uint8_t>((v >>  8) & 0xFF);
        out[7] = static_cast<uint8_t>( v        & 0xFF);
        return 8;
    }
    return 0;  // out of range
}

size_t VarintDecode(const uint8_t* in, size_t in_size, uint64_t* value) {
    if (in_size < 1) return 0;
    const uint8_t prefix = in[0] >> 6;
    const size_t len = (prefix == 0) ? 1 : (prefix == 1) ? 2 :
                       (prefix == 2) ? 4 : 8;
    if (in_size < len) return 0;

    uint64_t v = in[0] & 0x3F;
    for (size_t i = 1; i < len; ++i) {
        v = (v << 8) | in[i];
    }
    *value = v;
    return len;
}

}  // namespace lumen
