import { describe, it, expect } from 'vitest';
import {
    HEADER_SIZE,
    FLAG_KEYFRAME,
    FLAG_LAST_FRAGMENT,
    serializeHeader,
    deserializeHeader,
} from './packet_header.js';

// Golden bytes computed from the layout in
// src/transport/src/media_packet_header.cpp (network byte order).
//
// Field values:
//   ssrc           = 0x12345678
//   frameIndex     = 0x0102030405060708n
//   timestampUs    = 0x1112131415161718n
//   fragmentIndex  = 0x0001
//   fragmentCount  = 0x0002
//   isKeyframe     = true   -> flags bit 0
//   isLastFragment = true   -> flags bit 1
//   flags          = 0x00000003
const GOLDEN_HEADER = {
    ssrc:           0x12345678,
    frameIndex:     0x0102030405060708n,
    timestampUs:    0x1112131415161718n,
    fragmentIndex:  0x0001,
    fragmentCount:  0x0002,
    isKeyframe:     true,
    isLastFragment: true,
};

const GOLDEN_BYTES = new Uint8Array([
    0x12, 0x34, 0x56, 0x78,                          // ssrc
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,  // frameIndex
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,  // timestampUs
    0x00, 0x01,                                      // fragmentIndex
    0x00, 0x02,                                      // fragmentCount
    0x00, 0x00, 0x00, 0x03,                          // flags
]);

describe('MediaPacketHeader wire format', () => {
    it('HEADER_SIZE matches C++ kSerializedSize', () => {
        expect(HEADER_SIZE).toBe(28);
    });

    it('serializes to the golden byte sequence', () => {
        const buf = new Uint8Array(HEADER_SIZE);
        serializeHeader(buf, GOLDEN_HEADER);
        expect(Array.from(buf)).toEqual(Array.from(GOLDEN_BYTES));
    });

    it('round-trips serialize → deserialize', () => {
        const buf = new Uint8Array(HEADER_SIZE);
        serializeHeader(buf, GOLDEN_HEADER);
        const decoded = deserializeHeader(buf);
        expect(decoded).toEqual(GOLDEN_HEADER);
    });

    it('flags bits are independent', () => {
        const buf = new Uint8Array(HEADER_SIZE);

        serializeHeader(buf, { ...GOLDEN_HEADER,
                               isKeyframe: true, isLastFragment: false });
        expect(buf[27]).toBe(FLAG_KEYFRAME);

        serializeHeader(buf, { ...GOLDEN_HEADER,
                               isKeyframe: false, isLastFragment: true });
        expect(buf[27]).toBe(FLAG_LAST_FRAGMENT);

        serializeHeader(buf, { ...GOLDEN_HEADER,
                               isKeyframe: false, isLastFragment: false });
        expect(buf[27]).toBe(0);
    });

    it('accepts plain Number for uint64 fields', () => {
        const buf1 = new Uint8Array(HEADER_SIZE);
        const buf2 = new Uint8Array(HEADER_SIZE);
        serializeHeader(buf1, { ...GOLDEN_HEADER,
                                frameIndex: 42, timestampUs: 1_000_000 });
        serializeHeader(buf2, { ...GOLDEN_HEADER,
                                frameIndex: 42n, timestampUs: 1_000_000n });
        expect(Array.from(buf1)).toEqual(Array.from(buf2));
    });

    it('throws on undersized buffer', () => {
        expect(() => serializeHeader(new Uint8Array(27), GOLDEN_HEADER))
            .toThrow(RangeError);
        expect(() => deserializeHeader(new Uint8Array(27)))
            .toThrow(RangeError);
    });

    it('respects byte offset within a larger buffer', () => {
        const big = new Uint8Array(64);
        const slice = new Uint8Array(big.buffer, 16, HEADER_SIZE);
        serializeHeader(slice, GOLDEN_HEADER);

        const direct = new Uint8Array(HEADER_SIZE);
        serializeHeader(direct, GOLDEN_HEADER);

        for (let i = 0; i < HEADER_SIZE; ++i) {
            expect(big[16 + i]).toBe(direct[i]);
        }
    });
});
