// Lumen web sender SDK — public entry.
//
// Primary API: LumenSender (encode a caller-provided MediaStream → WebCodecs →
// Lumen wire framing → WebTransport, with reconnect/error recovery and event
// callbacks). The SDK never captures media or touches the DOM; the consuming
// app owns getDisplayMedia/getUserMedia and the UI. See examples/screen-share.

export { LumenSender } from './lumen_sender.js';

// Session helpers (fetch the receiver's transport descriptor).
export { fetchSession, openTransport } from './sender/transport.js';

// Lower-level building blocks for advanced consumers.
export { fragment, MAX_PAYLOAD } from './sender/fragmenter.js';
export {
    HEADER_SIZE,
    serializeHeader,
    deserializeHeader,
    FLAG_KEYFRAME,
    FLAG_LAST_FRAGMENT,
} from './wire/packet_header.js';
export {
    CHANNEL_VIDEO,
    CHANNEL_AUDIO,
    CHANNEL_CONTROL,
} from './wire/channel.js';
