// Splits an encoded frame into WebTransport datagrams.
//
// Each datagram is: [channel_id:u8][MediaPacketHeader:28][payload slice],
// matching the native receiver's wire format (see wire_format.h +
// media_packet_header.cpp). Pure function — unit-tested in fragmenter.test.js.

import { HEADER_SIZE, serializeHeader } from '../wire/packet_header.js';

// Keep each datagram under quiche's ~1.2 KB WT datagram limit (channel byte +
// 28-byte header + payload).
export const MAX_PAYLOAD = 1100;

/**
 * Fragment an encoded frame.
 * @returns {Uint8Array[]} one Uint8Array per datagram, in fragment order.
 */
export function fragment({
    channel,
    ssrc,
    frameIndex,
    timestampUs,
    isKeyframe,
    payload,
    maxPayload = MAX_PAYLOAD,
}) {
    const total = Math.max(1, Math.ceil(payload.byteLength / maxPayload));
    const out = [];
    for (let i = 0; i < total; ++i) {
        const start = i * maxPayload;
        const end = Math.min(start + maxPayload, payload.byteLength);
        const slice = payload.subarray(start, end);

        const dgram = new Uint8Array(1 + HEADER_SIZE + slice.byteLength);
        dgram[0] = channel;
        serializeHeader(dgram.subarray(1, 1 + HEADER_SIZE), {
            ssrc,
            frameIndex,
            timestampUs,
            fragmentIndex: i,
            fragmentCount: total,
            isKeyframe: isKeyframe && i === 0,
            isLastFragment: i === total - 1,
        });
        dgram.set(slice, 1 + HEADER_SIZE);
        out.push(dgram);
    }
    return out;
}
