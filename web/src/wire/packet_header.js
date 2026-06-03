// Wire format for the per-fragment header that prefixes every media payload.
// Mirrors `MediaPacketHeader::Serialize` in
// src/transport/src/media_packet_header.cpp — bit-for-bit identical.
//
// Layout (28 bytes, network byte order):
//   [0..3]   ssrc            uint32
//   [4..11]  frameIndex      uint64
//   [12..19] timestampUs     uint64
//   [20..21] fragmentIndex   uint16
//   [22..23] fragmentCount   uint16
//   [24..27] flags           uint32
//                              bit 0      = isKeyframe
//                              bit 1      = isLastFragment
//                              bits 2-27  = reserved (must be 0)
//                              bits 28-31 = wire-format version (currently 0)

export const HEADER_SIZE = 28;

export const FLAG_KEYFRAME = 0x1;
export const FLAG_LAST_FRAGMENT = 0x2;

export const VERSION_SHIFT = 28;
export const VERSION_MASK = 0xF << VERSION_SHIFT;  // bits 28-31
export const CURRENT_VERSION = 0;

const BIG_ENDIAN = false;

function toBigUint64(v) {
    return typeof v === 'bigint' ? v : BigInt(v);
}

/// Serialize `hdr` into the first 28 bytes of `buf` (a Uint8Array).
/// `buf.byteLength` must be >= HEADER_SIZE.
export function serializeHeader(buf, hdr) {
    if (buf.byteLength < HEADER_SIZE) {
        throw new RangeError(`buf too small: ${buf.byteLength} < ${HEADER_SIZE}`);
    }
    const view = new DataView(buf.buffer, buf.byteOffset, HEADER_SIZE);
    view.setUint32(0, hdr.ssrc >>> 0, BIG_ENDIAN);
    view.setBigUint64(4, toBigUint64(hdr.frameIndex), BIG_ENDIAN);
    view.setBigUint64(12, toBigUint64(hdr.timestampUs), BIG_ENDIAN);
    view.setUint16(20, hdr.fragmentIndex & 0xFFFF, BIG_ENDIAN);
    view.setUint16(22, hdr.fragmentCount & 0xFFFF, BIG_ENDIAN);

    let flags = 0;
    if (hdr.isKeyframe)     flags |= FLAG_KEYFRAME;
    if (hdr.isLastFragment) flags |= FLAG_LAST_FRAGMENT;
    const version = (hdr.version ?? CURRENT_VERSION) & 0xF;
    flags |= version << VERSION_SHIFT;
    view.setUint32(24, flags >>> 0, BIG_ENDIAN);
}

/// Deserialize a header from the first 28 bytes of `buf`. Returns an object
/// with the same shape `serializeHeader` consumes. uint64 fields are BigInts.
export function deserializeHeader(buf) {
    if (buf.byteLength < HEADER_SIZE) {
        throw new RangeError(`buf too small: ${buf.byteLength} < ${HEADER_SIZE}`);
    }
    const view = new DataView(buf.buffer, buf.byteOffset, HEADER_SIZE);
    const flags = view.getUint32(24, BIG_ENDIAN);
    return {
        ssrc:           view.getUint32(0, BIG_ENDIAN),
        frameIndex:     view.getBigUint64(4, BIG_ENDIAN),
        timestampUs:    view.getBigUint64(12, BIG_ENDIAN),
        fragmentIndex:  view.getUint16(20, BIG_ENDIAN),
        fragmentCount:  view.getUint16(22, BIG_ENDIAN),
        isKeyframe:     (flags & FLAG_KEYFRAME) !== 0,
        isLastFragment: (flags & FLAG_LAST_FRAGMENT) !== 0,
        version:        (flags & VERSION_MASK) >>> VERSION_SHIFT,
    };
}
