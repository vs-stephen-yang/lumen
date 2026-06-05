import { describe, it, expect } from 'vitest';
import { fragment, MAX_PAYLOAD } from './fragmenter.js';
import { HEADER_SIZE, deserializeHeader } from '../wire/packet_header.js';
import { CHANNEL_VIDEO } from '../wire/channel.js';

function bodyHeader(dgram) {
    // Strip the leading channel_id byte, then parse the 28-byte header.
    return deserializeHeader(dgram.subarray(1, 1 + HEADER_SIZE));
}
function bodyPayload(dgram) {
    return dgram.subarray(1 + HEADER_SIZE);
}

describe('fragmenter', () => {
    const base = {
        channel: CHANNEL_VIDEO,
        ssrc: 0x10000001,
        frameIndex: 7n,
        timestampUs: 123456n,
        isKeyframe: true,
    };

    it('single fragment for a small frame', () => {
        const payload = new Uint8Array(500).fill(0xAB);
        const frags = fragment({ ...base, payload });
        expect(frags.length).toBe(1);
        const d = frags[0];
        expect(d[0]).toBe(CHANNEL_VIDEO);
        expect(d.byteLength).toBe(1 + HEADER_SIZE + 500);
        const h = bodyHeader(d);
        expect(h.fragmentIndex).toBe(0);
        expect(h.fragmentCount).toBe(1);
        expect(h.isKeyframe).toBe(true);
        expect(h.isLastFragment).toBe(true);
        expect(h.frameIndex).toBe(7n);
    });

    it('splits a large frame at the MTU and reassembles intact', () => {
        const size = MAX_PAYLOAD * 2 + 137;  // 3 fragments
        const payload = new Uint8Array(size);
        for (let i = 0; i < size; ++i) payload[i] = (i * 31 + 7) & 0xFF;

        const frags = fragment({ ...base, payload });
        expect(frags.length).toBe(3);

        const reassembled = [];
        frags.forEach((d, i) => {
            const h = bodyHeader(d);
            expect(h.fragmentIndex).toBe(i);
            expect(h.fragmentCount).toBe(3);
            // keyframe flag only on the first fragment
            expect(h.isKeyframe).toBe(i === 0);
            expect(h.isLastFragment).toBe(i === 2);
            reassembled.push(...bodyPayload(d));
        });
        expect(reassembled.length).toBe(size);
        expect(Uint8Array.from(reassembled)).toEqual(payload);
    });

    it('always emits at least one datagram for an empty payload', () => {
        const frags = fragment({ ...base, payload: new Uint8Array(0) });
        expect(frags.length).toBe(1);
        expect(frags[0].byteLength).toBe(1 + HEADER_SIZE);
        expect(bodyHeader(frags[0]).isLastFragment).toBe(true);
    });
});
