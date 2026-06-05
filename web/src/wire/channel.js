// Canonical multiplexing channel ids — the leading byte of every WT datagram
// payload: [channel_id:u8][MediaPacketHeader:28][fragment].
//
// MUST match wire::kChannel* in
// src/transport/include/lumen/transport/wire_format.h (the native receiver
// dispatches on these exact values).
export const CHANNEL_VIDEO = 0;
export const CHANNEL_AUDIO = 1;
export const CHANNEL_CONTROL = 2;
