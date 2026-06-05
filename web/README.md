# Lumen Web Sender SDK (`@lumen/web-sender`)

Browser-side SDK that streams a `MediaStream` to the native Lumen receiver:
WebCodecs encode → Lumen wire framing → WebTransport datagrams, with built-in
reconnect / error recovery.

This is an **SDK** — it does not capture media or render UI. The consuming app
owns `getDisplayMedia`/`getUserMedia` and the UI and passes the stream in. See
`examples/screen-share/` for a working consumer.

## Usage

```js
import { LumenSender } from '@lumen/web-sender';   // -> src/index.js

const stream = await navigator.mediaDevices.getDisplayMedia({ video: true, audio: true });
const sender = new LumenSender({
    stream,                       // caller-owned capture
    // session,                   // optional; else fetched from sessionUrl
    sessionUrl: '/api/session',   // receiver's transport descriptor
    video: { bitrate: 2_500_000 },
    hooks: {
        onStatus: (s) => {},      // 'connecting'|'live'|'reconnecting'|'stopped'|'error'
        onStats:  (s) => {},      // { video, audio, fps, dt }
        onLog:    (m) => {},
        onError:  (e) => {},
    },
});
await sender.start();
// … later
await sender.stop();
```

The SDK reconnects with exponential backoff on transport loss (and forces a
keyframe after reconnect so the decoder resyncs), recreates a failed encoder,
and stops cleanly when the capture track ends ("Stop sharing").

Also exported: `fetchSession`, `openTransport`, and wire primitives
(`fragment`, `serializeHeader`/`deserializeHeader`, `CHANNEL_*`).

## Layout

```
src/                 SDK (library)
  index.js           public entry
  lumen_sender.js    LumenSender
  sender/            encoders, fragmenter, transport, reconnect (pure)
  wire/              packet_header, channel  (mirror src/transport wire_format.h)
examples/
  screen-share/      example app that consumes the SDK
```

## Scripts

```sh
npm test         # vitest — pure logic (wire header, fragmenter, backoff)
npm run dev      # vite dev server for examples/screen-share
npm run build    # build examples/screen-share -> examples/screen-share/dist
```

`web_receiver` serves the built example at `/sender/` (its POST_BUILD deploys
`examples/screen-share/dist` when present, else the legacy test page).

## Testing the full path

- **Automated** (`tests/web_e2e_test`): drives the example app with
  `?source=camera` (Chrome fake device, deterministic 30fps) → real
  WebTransport → native receiver, asserting `received == decoded`.
- **Manual screen share**: build (`npm run build`), run `web_receiver`, open
  `http://localhost:8080/sender/` in Chrome/Edge, click **Start screen share**,
  pick a screen. The receiver logs `[fps]`; the page title shows `OK`. To test
  recovery, restart `web_receiver` mid-share — the sender reconnects and resumes.
  (Headless `getDisplayMedia` yields a near-static surface, so the screen path
  is not automated.)
```
