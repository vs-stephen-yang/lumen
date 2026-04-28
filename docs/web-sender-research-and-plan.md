# Web Sender — Research, Design, and E2E Test Plan

## 1. Overview

This document covers a **browser-based sender** for Lumen: a web page that
captures the user's screen + system audio, encodes to H.264 + Opus, and
streams to a Lumen receiver — **without using WebRTC**.

The first concrete deliverable is an **end-to-end test**: a web page in
Chromium streams to a Windows Lumen receiver running the existing decode +
render pipeline. A small signaling endpoint on the receiver hands the browser
the connection details.

### Why no WebRTC?

The reasons we have for avoiding WebRTC for this path:

- **Existing transport already mirrors WebRTC's data layer.** Lumen has its
  own `MediaPacketHeader`, fragmenter/reassembler, TCP and QUIC backends, and
  congestion stats. Adding a WebRTC stack would duplicate those.
- **WebRTC is opinionated about media.** The PeerConnection pipeline owns the
  encoder choice, congestion control, jitter buffer, and packetization
  (RTP/SRTP). We want the encoder, framing, and pacing under our control to
  stay consistent with the native sender.
- **No need for ICE / DTLS / SRTP.** Our scenarios are 1:1 with a known
  receiver address (LAN, same machine, or a relay we operate). The full
  WebRTC stack is overhead we don't get value from.
- **WebCodecs + WebTransport now cover the gap.** Both are in Baseline as of
  Q1 2026 and let us build a sender on the same primitives the C++ side uses,
  without a PeerConnection.

### Goals

| # | Goal | Measure |
|---|---|---|
| G1 | Capture screen + system audio in browser | `getDisplayMedia({video, audio})` returns both tracks on Chrome/Edge |
| G2 | Encode H.264 + Opus in the browser | `VideoEncoder.isConfigSupported()` reports `hardwareAcceleration: 'prefer-hardware'` succeeds |
| G3 | Send to a Windows receiver over a non-WebRTC transport | Receiver logs accepted frames, decode + render works |
| G4 | Reuse the existing `MediaPacketHeader` wire format | Web sender's bytes are bit-for-bit identical to the C++ sender's |
| G5 | A simple signaling channel for connection setup | Receiver exposes one HTTP endpoint; sender pulls config and connects |
| G6 | E2E test that runs in CI (headless) | Single command starts receiver + headless browser sender; passes/fails on frame counts and decode |

### Non-goals (this phase)

- Multi-party / SFU
- TURN relay or NAT traversal beyond same-LAN / localhost
- Production-grade signaling (auth, room management, lobby)
- Browser-side decoding / rendering (sender-only for now)
- Mobile browser support — Chrome/Edge desktop is the bar for the E2E test

---

## 2. Web Capture Layer

### 2.1 API choice

| API | Use | Status |
|---|---|---|
| `navigator.mediaDevices.getDisplayMedia({video, audio})` | Screen + system audio capture | Stable, Baseline |
| `MediaStreamTrackProcessor` | Stream of raw `VideoFrame` / `AudioData` per track | Chrome 94+ stable, Firefox 130+ behind flag, Safari 17+ |
| `MediaRecorder` | Container output (webm/mp4) | Stable, but **wrong abstraction** — produces a muxed container, opaque encoder choice, higher latency |

**Recommendation:** `getDisplayMedia` → `MediaStreamTrackProcessor` →
`WebCodecs`. MediaRecorder is a non-starter for low-latency streaming
because we cannot get individual encoded frames out of it without re-parsing
the container, and we cannot tune encoder parameters per frame.

### 2.2 System audio support

This is the sharpest browser-compat constraint:

| Browser | OS | System audio via `getDisplayMedia` |
|---|---|---|
| Chrome | Windows | **Yes** (since Chrome 74) |
| Chrome | ChromeOS / Linux | Yes (tab/window only on Linux) |
| Chrome | macOS | Yes since Chrome 141 + macOS 14.2 |
| Edge | Windows | Yes (Chromium-based) |
| Firefox | All | **Silently dropped** — no error, no audio track |
| Safari | macOS / iOS | Tab audio only, no full-system audio |

**Implication for E2E test:** Target Chrome/Edge on Windows. Microphone
fallback (`getUserMedia({audio: true})`) is the workaround for Firefox/Safari
but is out of scope here — the receiver is for system-audio screen sharing.

### 2.3 Frame extraction

```js
const stream = await navigator.mediaDevices.getDisplayMedia({
    video: { frameRate: 60, width: 1920, height: 1080 },
    audio: { sampleRate: 48000, channelCount: 2 },
    // Hint to allow system audio (Chrome-specific)
    systemAudio: 'include',
    selfBrowserSurface: 'exclude',
    surfaceSwitching: 'exclude',
});

const [videoTrack] = stream.getVideoTracks();
const [audioTrack] = stream.getAudioTracks();

const videoProcessor = new MediaStreamTrackProcessor({ track: videoTrack });
const audioProcessor = new MediaStreamTrackProcessor({ track: audioTrack });

// videoProcessor.readable → ReadableStream<VideoFrame>
// audioProcessor.readable → ReadableStream<AudioData>
```

Both readables are transferable to a Web Worker — and they should be, so
the encoder doesn't fight the main thread for budget.

---

## 3. Web Encoding Layer (WebCodecs)

### 3.1 Why WebCodecs

- Direct hardware access on every modern browser (Windows: Media Foundation;
  macOS: VideoToolbox; Android: MediaCodec). This is the same set of HW
  encoders the native Lumen sender uses.
- Returns raw `EncodedVideoChunk` / `EncodedAudioChunk` — we control framing,
  no container, no RTP.
- Async, off-main-thread when run in a Worker.

### 3.2 Video config (H.264 to start)

```js
const encoder = new VideoEncoder({
    output: (chunk, metadata) => sender.enqueueVideo(chunk, metadata),
    error: (err) => console.error('encoder error:', err),
});

await VideoEncoder.isConfigSupported({
    codec: 'avc1.640028',      // H.264 High Profile, level 4.0
    width: 1920,
    height: 1080,
    framerate: 60,
    bitrate: 8_000_000,
    bitrateMode: 'constant',
    latencyMode: 'realtime',
    hardwareAcceleration: 'prefer-hardware',
    avc: { format: 'annexb' }, // matches what MFT decoder expects
});

encoder.configure({ /* same fields */ });
```

Notes:

- `latencyMode: 'realtime'` disables look-ahead and B-frames — required for
  low-latency screen sharing.
- `avc.format: 'annexb'` produces start-code framed NAL units, which the
  Windows `MfVideoDecoder` already accepts. Avoids an extra repack step.
- `hardwareAcceleration: 'prefer-hardware'` — the encoder *may* fall back to
  software if HW is unavailable; we should refuse to start if
  `isConfigSupported(...).config.hardwareAcceleration !== 'prefer-hardware'`
  resolves to a software config, since that won't sustain 60fps@1080p.
- HEVC + AV1 are next-stage codec options; feature-gate at startup.

### 3.3 Audio config (Opus)

```js
const audioEncoder = new AudioEncoder({
    output: (chunk, metadata) => sender.enqueueAudio(chunk, metadata),
    error: (err) => console.error('audio encoder error:', err),
});

audioEncoder.configure({
    codec: 'opus',
    sampleRate: 48000,
    numberOfChannels: 2,
    bitrate: 128_000,
    opus: {
        application: 'lowdelay',  // matches OPUS_APPLICATION_RESTRICTED_LOWDELAY
        frameDuration: 20_000,    // µs — matches our 20ms frame size
        useinbandfec: false,
        usedtx: false,
    },
});
```

The C++ `OpusAudioDecoder` is symmetric: same sample rate, same channel
count, same frame size. No bridge code needed beyond passing the raw Opus
packet bytes through.

### 3.4 Worker layout

```
Main thread
└─ getDisplayMedia, signaling, UI
   └─ MediaStreamTrackProcessor (transferred to worker)

Encoder Worker
├─ ReadableStream<VideoFrame>  → VideoEncoder → EncodedVideoChunk
├─ ReadableStream<AudioData>   → AudioEncoder → EncodedAudioChunk
└─ → Sender (postMessage to network worker, or same worker)

Network Worker
└─ Framer (MediaPacketHeader) → Fragmenter → WebTransport / WebSocket
```

Two workers (encoder + network) is a clean split, but a single worker is
fine for v1 — keep it simple and measure before splitting.

---

## 4. Transmission Protocol Comparison

### 4.1 Comparison matrix

| Protocol | Underlying | Reliable | Unreliable | Streams | Browser API | Server need | Lumen reuse | Verdict |
|---|---|---|---|---|---|---|---|---|
| **WebSocket** | TCP + TLS | Yes | No | One stream / connection | `WebSocket` | TCP listener + HTTP upgrade | Builds on existing TCP backend | Phase 1 |
| **WebTransport** | HTTP/3 over QUIC | Yes (streams) | Yes (datagrams) | Many streams + datagrams | `WebTransport` | HTTP/3 server (e.g. libwtf on msquic) | Builds on existing QUIC backend | Phase 2 (target) |
| **fetch streaming** (POST + ReadableStream upload) | HTTP/2 or HTTP/3 | Yes | No | One per request | `fetch` with body stream | HTTP/2+ server | Less suited to long-lived bidi | No |
| **Server-Sent Events** | HTTP/1.1+ | Yes | No | Receive-only | `EventSource` | HTTP server | One-direction only | No |
| **WebRTC DataChannel** | SCTP over DTLS over UDP | Optional | Optional | Many | `RTCDataChannel` | ICE/DTLS/SCTP | Excluded by goal | Excluded |
| **Raw UDP** | UDP | — | Yes | — | **Not exposed in browsers** | — | — | Impossible |
| **Raw TCP** | TCP | Yes | No | One | **Not exposed in browsers** | — | — | Impossible |

### 4.2 The two real choices

#### WebSocket (over TCP)

**Pros**
- Universal: every browser, every proxy, every corporate network.
- Cheap to add to the receiver — the existing `TcpTransportServer` already
  speaks raw TCP; an HTTP/1.1 upgrade handshake on top is ~150 lines.
- Each WS message can be one fragment (`MediaPacketHeader` + payload). Maps
  cleanly onto `FrameReassembler::AddFragment(packet, len)`.
- TLS via `wss://` is the same TLS the existing TCP backend would use.

**Cons**
- TCP head-of-line blocking: a single dropped packet stalls all video and
  audio behind it. Bad for audio in particular — a 200 ms TCP retransmit
  shows up as a 200 ms audio gap. Audio cannot use unreliable delivery.
- Bandwidth feedback is poor: TCP gives us RTT but no easy `is_congested` /
  `bytes_in_flight` signal for the encoder's adaptive bitrate.

#### WebTransport (over HTTP/3 / QUIC)

**Pros**
- **Streams + datagrams in the same connection.** Video on a stream
  (reliable, ordered), audio on datagrams (unreliable, low-latency) — same
  shape as the native QUIC backend. Lossy audio uses Opus PLC, which is
  exactly what we already plan for.
- No head-of-line blocking across streams. A stalled video stream doesn't
  delay control messages.
- Native congestion stats (RTT, BBR / CUBIC state) — QUIC exposes what TCP
  hides.
- Connection migration: a sender on a flaky Wi-Fi connection can survive
  IP changes.
- Mandatory TLS 1.3.

**Cons**
- Requires HTTP/3 server. msquic alone doesn't speak HTTP/3 — we'd add
  either:
  - **libwtf** (`andrewmd5/libwtf`) — C library implementing WebTransport
    on top of msquic. Closest fit; we already have msquic in the build.
  - **libwebtransport** (`tensor-language/libwebtransport`) — pure C/C++,
    self-contained.
  - Roll our own WebTransport framing on top of msquic + nghttp3.
- Browser requires HTTPS with a trusted certificate. For E2E with
  self-signed certs we use Chrome's
  `--ignore-certificate-errors-spki-list=<sha256>` flag, which is supported
  for headless runs.
- Datagram MTU is ~1.2 kB — same constraint we already deal with in the
  QUIC backend (`max_payload_size = 1200` in `TransportConfig`).

### 4.3 Recommendation

**Build WebSocket first, target WebTransport second.**

- WebSocket reuses the existing `TcpTransportServer` and ships in days, not
  weeks. It's the right vehicle for the first E2E test.
- WebTransport is the architecturally correct answer because it matches the
  channel split (video stream + audio datagram) the C++ pipeline already
  uses. Layer it in once the Phase-1 WebSocket path is green.
- Both backends sit behind the existing `TransportServer` /
  `TransportConnection` / `TransportChannel` interfaces. The web-sender side
  is a JS module with two implementations behind a `Transport` interface.

---

## 5. Wire Format Reuse

### 5.1 `MediaPacketHeader` in JavaScript

The native header is 28 bytes, network byte order
(`src/transport/src/media_packet_header.cpp:48-62`):

```
[0..3]   ssrc            uint32
[4..11]  frame_index     uint64
[12..19] timestamp_us    uint64
[20..21] fragment_index  uint16
[22..23] fragment_count  uint16
[24..27] flags           uint32   (bit 0 = is_keyframe, bit 1 = is_last_fragment)
```

JS implementation (one screenful, lives in `web/src/wire/packet_header.js`):

```js
export const HEADER_SIZE = 28;

export function serializeHeader(buf, hdr) {
    const view = new DataView(buf.buffer, buf.byteOffset, HEADER_SIZE);
    view.setUint32(0, hdr.ssrc, false);          // false = big-endian
    setBigUint64(view, 4, hdr.frameIndex);
    setBigUint64(view, 12, hdr.timestampUs);
    view.setUint16(20, hdr.fragmentIndex, false);
    view.setUint16(22, hdr.fragmentCount, false);
    let flags = 0;
    if (hdr.isKeyframe)     flags |= 0x1;
    if (hdr.isLastFragment) flags |= 0x2;
    view.setUint32(24, flags, false);
}
```

Validation: a unit test serializes the same struct in JS and in C++ (compile
the existing `MediaPacketHeader::Serialize` into a Node addon or just
hard-code one expected byte sequence) and asserts equality.

### 5.2 Channel mapping

| Channel | WebSocket backend | WebTransport backend |
|---|---|---|
| Video | One WS message per fragment | Bidirectional stream, length-prefixed fragments |
| Audio | One WS message per fragment | Datagrams (one packet = one fragment, fits in MTU) |
| Control | One WS message per message | Bidirectional stream |

This is identical to the channel mapping `quic_channel.{h,cpp}` already
implements. The web sender's transport layer is a slimmer mirror of it.

### 5.3 H.264 framing alignment

WebCodecs `EncodedVideoChunk` with `avc.format: 'annexb'` produces NAL units
prefixed with `00 00 00 01`. `MfVideoDecoder` accepts Annex B input as long
as the input media type is configured with `MF_MT_MPEG2_FLAGS = 0`. **Action
item:** verify the current decoder configuration, switch if needed.

If the decoder is already configured for AVCC (length-prefixed), we can ask
WebCodecs for AVCC instead — same byte content, different framing.

---

## 6. Signaling Channel Design

### 6.1 What signaling has to do

For a 1:1 sender → receiver with no NAT traversal:

1. **Discover endpoint** — the sender needs the receiver's transport URL
   (`wss://host:port/lumen` or `https://host:port/`).
2. **Agree on session** — a session ID so multiple browsers don't collide on
   the same receiver, and so the receiver knows which encoder config to
   accept.
3. **Exchange capabilities** — codec, resolution, FPS, bitrate ceiling.
4. **Authenticate** (production only, not for E2E) — a token that the
   receiver checks before accepting the transport connection.

For E2E #1 — #3 are enough.

### 6.2 Concrete signaling endpoint

Receiver runs a single HTTP server (using a small embedded HTTP library —
e.g. cpp-httplib, single-header) on, say, `:8080`:

```
GET  /sender              → static HTML+JS bundle (the web sender app)
GET  /api/session         → returns JSON:
                            {
                              "session_id": "uuid-v4",
                              "transport": {
                                "kind": "websocket",
                                "url":  "wss://localhost:8443/lumen",
                                "ssrc": 0x12345678
                              },
                              "video": { "codec": "avc1.640028",
                                         "width": 1920, "height": 1080,
                                         "fps": 60, "bitrate": 8000000 },
                              "audio": { "codec": "opus",
                                         "sampleRate": 48000,
                                         "channels": 2, "bitrate": 128000 }
                            }
POST /api/sender/{id}/ack → sender posts after it's connected and ready
```

The sender:
1. Loads `/sender`.
2. Calls `/api/session`, receives config.
3. Configures WebCodecs from the JSON.
4. Opens the transport URL.
5. POSTs `/ack` so the receiver can start its decode pipeline.

### 6.3 Why HTTP and not WebSocket for signaling

- It's two endpoints. We don't need bidirectional push for setup.
- Same HTTP server can also serve the static web sender bundle. One
  process, one port for the test.
- Keeps signaling stateless and trivial to swap for a real broker later.

### 6.4 What the C++ side needs

A ~200-line `SignalingServer` class:

- Owns a `cpp-httplib` `Server` instance.
- Routes `/api/session` and serves `web/dist/` statically.
- Holds a `std::map<SessionId, SessionConfig>` and exposes
  `WaitForSession()` / `GetActiveSessionConfig()` for the receiver loop.

This module lives in `src/signaling/` so it stays platform-agnostic and is
trivially mockable.

---

## 7. Windows Receiver Changes

### 7.1 Phase 1 — WebSocket on top of `TcpTransportServer`

The minimum viable change to accept a browser sender:

1. **`WebSocketTransportServer`** (new) implements `TransportServer`. Wraps
   the existing `TcpTransportServer` and adds:
   - HTTP/1.1 Upgrade handshake (`Sec-WebSocket-Key` →
     `Sec-WebSocket-Accept`).
   - WebSocket frame parser (FIN + opcode + masking key + payload length).
   - Each binary message is one `MediaPacketHeader`-prefixed fragment, fed
     directly into the existing `FrameReassembler`.
2. **TLS** — `wss://` requires TLS. We add Schannel TLS to the existing
   TCP backend (the certificate machinery from the QUIC backend in
   `quic_transport_server.cpp` for self-signed dev certs is reusable).
3. **No changes** to `TransportConnection`, `TransportChannel`,
   `FrameFragmenter`, or `FrameReassembler` — those work as-is.

Estimated size: ~600 LoC server + ~200 LoC frame parser + tests.

### 7.2 Phase 2 — WebTransport via libwtf (or libwebtransport)

1. Add `vcpkg` dependency for **libwtf** (or vendor it in `third_party/`).
2. **`WebTransportServer`** (new) implements `TransportServer`:
   - HTTP/3 listener via libwtf, which delegates to msquic underneath.
   - Sessions map onto our `TransportConnection`.
   - Bidi streams → video / control channels.
   - QUIC datagrams → audio channel.
3. Browser-side certificate trust: ship a script that prints the cert SPKI
   hash so users can pass `--ignore-certificate-errors-spki-list=<hash>` to
   Chrome (or install the dev CA).

The existing `quic_transport_server.cpp` already wraps msquic with the
right ownership patterns; the WebTransport server will look very similar
but with libwtf's session callbacks instead of raw QUIC stream callbacks.

### 7.3 Where the new code lives

```
src/
├── signaling/
│   ├── include/lumen/signaling/signaling_server.h
│   └── src/signaling_server.cpp           (HTTP, cross-platform)
└── transport/
    └── (no abstract changes — both are TransportServer impls)

platform/windows/transport/
├── ws_transport_server.{h,cpp}            (Phase 1)
├── ws_transport_connection.{h,cpp}
├── ws_transport_channel.{h,cpp}
├── webtransport_server.{h,cpp}            (Phase 2)
├── webtransport_connection.{h,cpp}
└── webtransport_channel.{h,cpp}

web/
├── package.json
├── vite.config.js
├── public/
│   └── index.html
└── src/
    ├── main.js                  (UI, getDisplayMedia, signaling)
    ├── workers/
    │   └── encoder_worker.js    (WebCodecs in worker)
    ├── wire/
    │   └── packet_header.js     (mirrors MediaPacketHeader)
    ├── transport/
    │   ├── transport.js         (interface)
    │   ├── ws_transport.js
    │   └── webtransport.js
    └── codec/
        ├── video_encoder.js
        └── audio_encoder.js
```

---

## 8. End-to-End Test Design

### 8.1 Architecture

```
┌─────────────────────────────────────────────┐
│ tests/web_e2e_test.cpp (test harness)       │
│  ├── Spawns receiver process                │
│  ├── Spawns headless Chrome                 │
│  ├── Waits for receiver frame counts        │
│  └── Asserts pass/fail                      │
└─────────────────────────────────────────────┘
            │                      │
            ▼                      ▼
   ┌────────────────┐     ┌────────────────────┐
   │ web_receiver   │     │ Chromium headless  │
   │ (process)      │     │ (Playwright)       │
   │                │     │                    │
   │ SignalingSrv   │◄────┤ GET /sender        │
   │  :8080  HTTP   │     │ GET /api/session   │
   │                │     │                    │
   │ WSTransportSrv │◄────┤ wss://...:8443     │
   │  :8443  WSS    │     │                    │
   │                │     │                    │
   │ Decode→Render  │     │ getDisplayMedia    │
   │ (or count-only │     │   →WebCodecs       │
   │  in headless)  │     │   →WS transport    │
   └────────────────┘     └────────────────────┘
```

The receiver runs in `count-only` mode for headless CI: skips
`D3D11SwapChainRenderer` (it needs a display), still runs decode and counts
decoded frames + audio packets.

### 8.2 Headless capture concern

`getDisplayMedia` normally requires a user gesture and a picker UI.
Chromium has flags that make it work non-interactively, which Playwright /
direct CDP can drive:

- `--auto-select-desktop-capture-source=Entire screen`
- `--use-fake-ui-for-media-stream`
- `--enable-features=GetDisplayMediaSetAutoSelectAllScreens`
- `--ignore-certificate-errors-spki-list=<sha256>` for the self-signed cert

For audio in headless mode there is no real system audio — we'll either
inject a `MediaStreamTrackGenerator`-fed test tone, or skip the audio
assertion in CI (matching how `e2e_pipeline_test.cpp:418` handles missing
audio capture today, but with an explicit `--no-audio` flag rather than a
silent skip).

### 8.3 Test harness (C++)

```cpp
// tests/web_e2e_test.cpp
//
// 1. Start Receiver with --port=8080 --transport-port=8443 --count-only
// 2. Wait for "ready" log line.
// 3. Launch headless Chrome with --remote-debugging-port=9222 and the
//    flags above, navigated to http://127.0.0.1:8080/sender.
// 4. Drive the page via CDP: click "Start" (or auto-start on load).
// 5. Poll receiver's /api/stats until decoded_frames >= N or timeout.
// 6. Assert decoded_frames in expected range, decode_errors == 0.
```

The harness can use `Bash`-style process spawning from a test util we
already have (the existing `e2e_pipeline_test.cpp` does in-process; this
one needs out-of-process for the browser).

### 8.4 What it actually validates

| Check | Threshold |
|---|---|
| Sender connected over WS / WebTransport | `connection_count >= 1` |
| Frames received (TCP/WS reliable) | `received >= sent * 0.99` |
| Frames decoded | `decoded >= received - 5` (pipeline fill) |
| No decode errors | `decode_errors == 0` |
| End-to-end latency P95 | `< 200 ms` over loopback |
| Audio packets received | `audio_packets >= expected * 0.95` (datagrams in Phase 2) |

The latency histogram code already exists in `e2e_pipeline_test.cpp:209-251`
— port it into the receiver and expose via `/api/stats`.

### 8.5 CI integration

- One CMake target: `lumen_web_e2e_test`.
- Pre-step: `npm run build` in `web/` produces `web/dist/`.
- Receiver serves `web/dist/` directly. No nginx, no extra container.
- Skip on platforms without Chromium installed (warn cleanly, don't fail).

---

## 9. Phased Implementation Plan

### Phase 1 — WebSocket loop (target: works end-to-end)

1. **Web sender skeleton** (`web/`): Vite + vanilla JS. No framework. Pages
   served from `web/dist/`.
2. **Wire format module** (`web/src/wire/packet_header.js`) + golden-bytes
   test against the C++ serializer.
3. **WebCodecs encoders** (`web/src/codec/`). Validate
   `isConfigSupported`. Throw on software fallback.
4. **WebSocket transport** (`web/src/transport/ws_transport.js`). Frames
   each encoded chunk through fragmenter → header → WS message.
5. **`SignalingServer`** (`src/signaling/`) — cross-platform.
   `cpp-httplib`. Static file routes + `/api/session`.
6. **`WebSocketTransportServer`** (`platform/windows/transport/`). HTTP
   upgrade + WS frame parser on top of the existing TCP server.
7. **TLS** in the WS server. Reuse the dev cert generation from
   `quic_transport_server.cpp`.
8. **Receiver entry point** (`platform/windows/apps/web_receiver/main.cpp`).
   Wires signaling + WS server + decode + render (or count-only mode).
9. **`tests/web_e2e_test.cpp`** with the headless flags above. Initially
   manual; add CDP later.

Definition of done: a developer can run `web_receiver.exe`, open
`http://localhost:8080/sender` in Chrome, hit Start, and see their screen
in the receiver window.

### Phase 2 — WebTransport (target: parity with native QUIC backend)

1. Vendor or vcpkg-add **libwtf**.
2. `WebTransportServer` / `Connection` / `Channel`. Mirrors the QUIC
   backend's split.
3. `web/src/transport/webtransport.js`. Streams for video/control,
   datagrams for audio.
4. Fingerprint / cert flag plumbing in the test harness.
5. Update `web_e2e_test` to run twice — once per transport backend — by
   parameterizing the signaling response.

Definition of done: same E2E test passes against `--transport=webtransport`,
and audio uses datagrams.

### Phase 3 — Production-readiness (out of scope here, listed for completeness)

- Token auth on signaling.
- Turn relay path (`turn:`) when LAN connection fails.
- Adaptive bitrate driven by WebTransport stats.
- HEVC + AV1 codec selection.
- Mobile browser testing.

---

## 10. Open Questions

1. **WebSocket framing on top of TCP backend.** Do we want a fully separate
   `WebSocketTransportServer` class, or a `WebSocketUpgradeShim` that runs
   in front of the existing TCP server and produces the same byte stream?
   The shim is smaller but couples the two.
2. **Encoder running in Worker vs main thread.** Workers add a postMessage
   per chunk. For 60fps@1080p that's 60 messages/s — small. But if we keep
   `VideoFrame` zero-copy across the worker boundary (`Transferable`), this
   is fine. Worth measuring.
3. **Decoder format alignment.** Annex B vs AVCC for `MfVideoDecoder` —
   verify before plumbing.
4. **Self-signed cert UX.** For dev, the `--ignore-certificate-errors`
   route works for headless. For interactive dev sessions, the user has to
   click through a cert warning. A 30-line script that adds the dev cert
   to the Windows root store would smooth this.
5. **Audio in headless CI.** A test tone via `MediaStreamTrackGenerator` is
   the cleanest path; a `--no-audio` flag is the laziest. Pick one.
6. **`MediaPacketHeader` versioning.** This plan assumes the header stays
   v0 (no version byte today — see `docs/review-followups.md`). If we add
   the version byte, the JS serializer must be updated in lockstep.

---

## 11. Summary

| Layer | Choice | Rationale |
|---|---|---|
| Capture | `getDisplayMedia` + `MediaStreamTrackProcessor` | Only viable path; gives raw `VideoFrame` / `AudioData` |
| Encode | WebCodecs (H.264 + Opus) | Hardware-accelerated; matches native sender's codecs |
| Transport (Phase 1) | WebSocket over TLS | Reuses existing TCP backend; ships fastest |
| Transport (Phase 2) | WebTransport over QUIC | Reuses existing QUIC backend's channel design; datagrams for audio |
| Wire format | Existing `MediaPacketHeader` | Bit-for-bit compatible with native sender |
| Signaling | One HTTP endpoint (`/api/session`) | Sufficient for 1:1; trivially swappable later |
| E2E test | Headless Chrome + receiver process | Single CMake target; runs in CI |

The end state is a **second sender frontend** for Lumen that uses the same
wire format, the same channel split, and the same receive-side pipeline
the native sender already uses — without bringing WebRTC into the build.
