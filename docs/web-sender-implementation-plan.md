# Web Sender — Implementation Plan

This document is the build plan for the design in
[`web-sender-research-and-plan.md`](./web-sender-research-and-plan.md).

It covers Phase 1 only: **WebSocket transport, screen + system audio, web
sender + Windows receiver, end-to-end test**. Phase 2 (WebTransport) is
deferred and only referenced where Phase-1 decisions affect it.

---

## 1. Approach

- **Reuse before build.** The existing C++ sender already establishes the
  channel split (video / audio / control), the wire header, the receive
  pipeline (decode + render), and the dev cert plumbing. The Phase-1 work is
  a new transport backend (WebSocket) that feeds the same channels, plus the
  matching JS sender, plus a tiny signaling server.
- **Vertical slice first.** Get a single H.264 frame from a Chrome page
  through to the C++ decoder before adding audio, fragmentation, stats, or
  TLS. Every step after that is an additive change with its own test.
- **One process for the receiver.** Signaling HTTP server + WebSocket media
  server + decode + render all live in `web_receiver.exe`. Single binary
  for dev, single subprocess for the test harness.
- **Test framework already chosen.** `int main()` + `check()` style, like
  `e2e_pipeline_test.cpp`. The new E2E test spawns subprocesses; existing
  helpers don't need to change.

---

## 2. Workstream dependency graph

```
            ┌──────────────────────────────────┐
            │  S0  Project scaffolding         │
            │      web/, src/signaling/, deps  │
            └──────────────────────────────────┘
                       │
        ┌──────────────┼───────────────────┐
        ▼              ▼                   ▼
┌──────────────┐ ┌──────────────┐ ┌────────────────────┐
│ S1 JS wire   │ │ S2 cpp-      │ │ S3 SignalingServer │
│    format    │ │    httplib   │ │    (cross-platform)│
│  golden test │ │    vendoring │ │                    │
└──────────────┘ └──────────────┘ └────────────────────┘
        │              │                   │
        ▼              ▼                   │
┌──────────────┐ ┌──────────────────┐      │
│ S4 WebCodecs │ │ S5 WebSocket     │      │
│    encoders  │ │    transport     │      │
│              │ │    (Windows)     │      │
└──────────────┘ └──────────────────┘      │
        │              │                   │
        └──────┬───────┘                   │
               ▼                           │
       ┌────────────────────┐              │
       │ S6 Web sender app  │              │
       │    (vertical slice)│              │
       └────────────────────┘              │
               │                           │
               └────────────┬──────────────┘
                            ▼
                ┌──────────────────────────┐
                │ S7 web_receiver.exe app  │
                └──────────────────────────┘
                            │
                            ▼
                ┌──────────────────────────┐
                │ S8 web_e2e_test (CMake)  │
                └──────────────────────────┘
                            │
                            ▼
                ┌──────────────────────────┐
                │ S9 TLS (wss://)          │
                │ S10 Audio path           │
                │ S11 Stats endpoint       │
                └──────────────────────────┘
```

S1 / S2 / S3 are independent and can be done in parallel. S4 and S5 can run
in parallel once their inputs land. S6 needs S1+S4+S5. S7 needs S3+S5. S8
needs S6+S7. S9–S11 are post-vertical-slice hardening.

---

## 3. Repo layout after Phase 1

```
lumen/
├── CMakeLists.txt
├── vcpkg.json                            # + cpp-httplib
├── third_party/
│   └── cpp-httplib/                      # vendored or via vcpkg
├── src/
│   └── signaling/
│       ├── CMakeLists.txt
│       ├── include/lumen/signaling/
│       │   ├── signaling_server.h
│       │   └── session_config.h
│       └── src/signaling_server.cpp
├── platform/windows/
│   ├── CMakeLists.txt
│   ├── apps/
│   │   └── web_receiver/
│   │       ├── CMakeLists.txt
│   │       └── main.cpp
│   └── transport/
│       ├── ws_transport_server.{h,cpp}
│       ├── ws_transport_connection.{h,cpp}
│       ├── ws_transport_channel.{h,cpp}
│       └── ws_frame_codec.{h,cpp}        # WebSocket frame parser/serializer
├── web/
│   ├── package.json
│   ├── vite.config.js
│   ├── public/
│   │   └── index.html
│   ├── src/
│   │   ├── main.js                       # UI, getDisplayMedia, signaling
│   │   ├── workers/encoder_worker.js
│   │   ├── wire/packet_header.js
│   │   ├── wire/packet_header.test.js
│   │   ├── transport/transport.js        # interface
│   │   ├── transport/ws_transport.js
│   │   ├── codec/video_encoder.js
│   │   └── codec/audio_encoder.js
│   └── dist/                             # build output, served by receiver
└── tests/
    ├── CMakeLists.txt
    ├── web_e2e_test.cpp
    └── helpers/
        ├── subprocess.h
        └── subprocess.cpp
```

---

## 4. Implementation steps

Each step has: **goal**, **files**, **acceptance criteria**, **test**.

### S0 — Project scaffolding

**Goal.** A buildable empty shell for both the C++ additions and the web
project.

**Files.**
- `web/package.json` — Vite, vanilla JS, vitest for unit tests.
- `web/vite.config.js` — output to `web/dist/`, base path `/`.
- `web/public/index.html` — empty placeholder.
- `src/signaling/CMakeLists.txt` — empty `lumen_signaling` static lib.
- `src/CMakeLists.txt` — `add_subdirectory(signaling)`.
- `platform/windows/apps/web_receiver/CMakeLists.txt` — empty
  `web_receiver` exe target with a `main.cpp` that prints "ok".
- `platform/windows/CMakeLists.txt` — `add_subdirectory(apps/web_receiver)`.

**Acceptance.** `cmake --build build` produces `web_receiver.exe` (which
prints "ok") and `npm --prefix web run build` produces `web/dist/`.

**Test.** None — a build is the test.

### S1 — JS wire format

**Goal.** A JS module that produces and consumes the **exact** byte layout
of `MediaPacketHeader`.

**Files.**
- `web/src/wire/packet_header.js` — `serializeHeader`, `deserializeHeader`,
  `HEADER_SIZE = 28`. Uses `DataView` with `littleEndian = false`.
- `web/src/wire/packet_header.test.js` — vitest. Encodes a fixed header
  and compares to a golden buffer copied byte-for-byte from a one-time
  C++ run (or computed inline using the documented layout).

**Acceptance.** `npm test` passes. The golden buffer matches the spec in
`media_packet_header.cpp:48-62`.

**Test.** vitest unit test plus a manual C++ ↔ JS check on first land:
hand-run a small C++ snippet that calls `MediaPacketHeader::Serialize` for
a known set of values and compare hex.

### S2 — Vendor cpp-httplib

**Goal.** A single-header HTTP server available to the C++ build.

**Files.**
- `vcpkg.json` — add `"cpp-httplib"` to `dependencies`.
- `src/signaling/CMakeLists.txt` — `find_package(httplib CONFIG REQUIRED)`,
  `target_link_libraries(lumen_signaling PUBLIC httplib::httplib)`.

**Acceptance.** A trivial `httplib::Server` instance compiles and links
inside `lumen_signaling`.

**Test.** The build is the test for this step.

### S3 — `SignalingServer`

**Goal.** Cross-platform HTTP server that serves the web sender bundle and
exposes one JSON endpoint.

**Files.**
- `src/signaling/include/lumen/signaling/session_config.h` — struct of
  `session_id`, `transport.url`, `transport.kind`, `ssrc`,
  `video.{codec,width,height,fps,bitrate}`,
  `audio.{codec,sample_rate,channels,bitrate}`.
- `src/signaling/include/lumen/signaling/signaling_server.h`:
  ```cpp
  class SignalingServer {
   public:
    Result<void> Initialize(uint16_t port, const std::string& static_root);
    Result<void> Start();
    void Stop();

    void SetSessionConfig(const SessionConfig& config);
    /// Blocks until a session has been issued (sender pulled config),
    /// or returns kTimeout.
    Result<SessionConfig> WaitForSessionPickup(uint32_t timeout_ms);
   private:
    httplib::Server http_;
    std::thread thread_;
    std::mutex mu_;
    std::optional<SessionConfig> issued_;
    std::condition_variable cv_;
  };
  ```
- `src/signaling/src/signaling_server.cpp` — routes:
  - `GET /` → 302 to `/sender`
  - `GET /sender` → static `<root>/index.html`
  - `GET /sender/*` → static under `<root>/`
  - `GET /api/session` → JSON of current `SessionConfig`; sets `issued_`
    and notifies CV.

**Acceptance.** Curl against a running instance returns expected JSON;
static files served correctly; `WaitForSessionPickup()` returns when the
JSON is fetched.

**Test.** `tests/signaling_server_test.cpp` — start server on ephemeral
port, hit it with `httplib::Client`, assert response.

### S4 — WebCodecs encoders (in worker)

**Goal.** Encoder worker accepts `VideoFrame` / `AudioData` from main, emits
`EncodedVideoChunk` / `EncodedAudioChunk` back via `postMessage`.

**Files.**
- `web/src/codec/video_encoder.js` — wraps `VideoEncoder`, validates with
  `isConfigSupported`, refuses software fallback.
- `web/src/codec/audio_encoder.js` — wraps `AudioEncoder`.
- `web/src/workers/encoder_worker.js` — receives transferred
  `MediaStreamTrackProcessor.readable`, pumps through encoders, posts
  `{type: 'video'|'audio', chunk: Uint8Array, metadata}` to main.

**Acceptance.** A driver page (manual) starts capture, transfers tracks to
the worker, logs `chunk.byteLength` for ~60 frames/s.

**Test.** vitest is impractical here (needs real `getDisplayMedia`); test
with a Playwright headed page that logs and asserts non-zero chunks.

### S5 — WebSocket transport (Windows)

**Goal.** A `TransportServer` implementation that accepts HTTP/1.1 clients
on a TCP port, performs the WebSocket upgrade, parses incoming WS frames,
and dispatches binary messages to channels.

**Wire convention** (mirrors `tcp_transport_channel.h:14-19` minus the
length prefix, since WebSocket already provides message boundaries):

```
WS binary message body:
  [channel_id:u8][MediaPacketHeader:28][frame payload]
```

`channel_id` values come from `ChannelType` (`kVideo=0, kAudio=1, kControl=2`).

**Files.**
- `platform/windows/transport/ws_frame_codec.h` / `.cpp` — pure functions
  to parse and emit RFC 6455 frames. Supports binary opcode, masking
  (client→server only), close, ping/pong. Tested with golden vectors.
- `platform/windows/transport/ws_transport_server.{h,cpp}` — analogous to
  `tcp_transport_server.{h,cpp}`. Runs an accept loop on `winsock2`. For
  each accepted socket, reads the HTTP upgrade request, validates
  `Sec-WebSocket-Key`, sends the upgrade response, then hands the socket
  to a `WsTransportConnection`.
- `platform/windows/transport/ws_transport_connection.{h,cpp}` — owns the
  socket and the WS frame codec. Receive loop reads frames, dispatches
  binary messages to channels. Send queue serializes and writes WS frames.
  Sends pong in response to ping.
- `platform/windows/transport/ws_transport_channel.{h,cpp}` —
  `TransportChannel` impl. `Send()` builds
  `[channel_id][header][frame]`, asks the connection to wrap as a binary
  WS message and queue.

**Acceptance.**
1. A `wscat`-style client connects to `ws://localhost:PORT/lumen` and
   receives the upgrade handshake.
2. A binary message echoed by the server arrives byte-identical.
3. A `MediaPacketHeader`-prefixed payload is delivered to the right
   channel and parses correctly downstream.

**Test.** `tests/ws_transport_test.cpp`:
- Start server on ephemeral port.
- Use a tiny in-test WS client (also implemented in `ws_frame_codec` —
  symmetric code) to send a known fragment.
- Assert that the channel's receive callback fired with matching bytes.

### S6 — Web sender app (vertical slice)

**Goal.** Open page → fetch session → start capture → encode → send video
frames to receiver. **Audio not yet wired.**

**Files.**
- `web/src/transport/transport.js` — interface (`open`, `sendVideo`,
  `sendAudio`, `close`, `onState`).
- `web/src/transport/ws_transport.js` — `WebSocket` impl. Opens
  `wss://`/`ws://` URL from session JSON; serializes
  `[channel_id][header][frame]` per chunk; sends as binary message.
- `web/src/main.js` —
  1. `fetch('/api/session')` → config.
  2. `getDisplayMedia` with the spec's `selfBrowserSurface: 'exclude'`
     and `systemAudio: 'include'`.
  3. Configure `VideoEncoder`. Start the worker.
  4. Open `WsTransport`. On encoder output, call `transport.sendVideo`.
  5. Auto-stop after `?frames=N` query param worth of chunks (lets the
     E2E test run a bounded amount of work).

**Acceptance.** Hand-run on Windows + Chrome: open page, see N video
chunks arrive at the receiver console.

**Test.** Manual at this step. Automation lands in S8.

### S7 — `web_receiver.exe`

**Goal.** Single binary that runs the signaling server, the WS transport
server, and the existing decode + render pipeline.

**Files.**
- `platform/windows/apps/web_receiver/main.cpp`:
  1. Parse `--http-port`, `--ws-port`, `--web-root`, `--frames`,
     `--render` (default true), `--audio` (default true).
  2. Build `SessionConfig` from CLI flags (or hard-coded defaults
     matching the existing pipeline: 1920×1080 @ 60fps, 8 Mbps,
     48 kHz / 2ch / 128 kbps Opus).
  3. Start `SignalingServer` on `--http-port` serving `--web-root`.
  4. Start `WsTransportServer` on `--ws-port`.
  5. On incoming connection, build the receive pipeline:
     `WsTransportChannel(kVideo)` → parse `MediaPacketHeader` → feed
     payload to `MfVideoDecoder` → `D3D11SwapChainRenderer` (if
     `--render`) or `null` sink (count-only).
  6. Same for audio (after S10).
  7. Stats: track `received_video_frames`, `decoded_frames`,
     `decode_errors`, latency histogram.
  8. Stop conditions: `--frames N` reached, or WS connection closes.

**Acceptance.** Hand-run end-to-end with the page from S6: open
`http://localhost:8080/sender`, see frames decoded and rendered (or
counted).

**Test.** Manual at this step.

### S8 — `web_e2e_test`

**Goal.** Single CMake test target that spawns receiver + headless
Chrome, asserts pass/fail.

**Files.**
- `tests/helpers/subprocess.{h,cpp}` — minimal cross-process helper:
  `Subprocess::Spawn(cmd, args, env)`, `WaitFor(timeout)`, `Kill()`,
  pipes for stdout/stderr to a string. Windows: `CreateProcess`.
- `tests/web_e2e_test.cpp`:
  ```cpp
  int main(int argc, char** argv) {
    const auto chrome = FindChrome(argv); // env var override, then PATH.
    if (chrome.empty()) {
      std::fprintf(stderr, "Chrome not found; skipping.\n");
      return 77; // standard "skip" exit code
    }

    auto receiver = Subprocess::Spawn("web_receiver.exe", {
      "--http-port=18080",
      "--ws-port=18443",
      "--web-root=" + WebDistPath(),
      "--frames=150",
      "--render=false",
      "--audio=false",                // until S10
    });
    WaitForLog(receiver, "ws server listening");

    auto chromiumArgs = std::vector<std::string>{
      "--headless=new",
      "--no-sandbox",
      "--disable-gpu",                // optional; HW accel still works for WebCodecs
      "--use-fake-ui-for-media-stream",
      "--auto-select-desktop-capture-source=Entire screen",
      "--enable-features=GetDisplayMediaSetAutoSelectAllScreens",
      "http://127.0.0.1:18080/sender?frames=150",
    };
    auto chrome = Subprocess::Spawn(chrome, chromiumArgs);

    auto stats = WaitForFinalStats(receiver, /*timeout=*/30'000);
    chrome.Kill();
    receiver.Kill();

    check(stats.received_video_frames >= 145, "frame count");
    check(stats.decode_errors == 0, "no decode errors");
    return 0;
  }
  ```
- `tests/CMakeLists.txt` — add the executable, depend on
  `web_receiver` (so it builds first), and a custom command that runs
  `npm --prefix web run build` before the test.

**Acceptance.** `ctest -R web_e2e` passes locally; skips with exit 77 if
Chrome isn't installed.

**Test.** This **is** the test.

### S9 — TLS (`wss://`)

**Goal.** Browser will refuse `ws://` on non-localhost. For a developer
that runs the receiver on another machine, we need TLS.

**Files.**
- `platform/windows/transport/ws_transport_server.cpp` — wrap accepted
  socket in Schannel (mirror the cert generation pattern from
  `quic_transport_server.cpp`).
- `web_receiver.exe` — `--cert <pfx>` and `--cert-thumbprint <sha1>`
  flags; if absent, generate a self-signed dev cert on startup and print
  the SPKI hash for the Chrome flag.

**Acceptance.** Page loaded over `https://localhost:18080/sender`
connects to `wss://localhost:18443/lumen` without errors.

**Test.** Extend `web_e2e_test` to flip a `--tls` switch and add the
SPKI flag to Chrome.

### S10 — Audio path

**Goal.** Web sender encodes Opus and sends; receiver decodes with
`OpusAudioDecoder`; renders via `WasapiAudioRenderer` (or skips on CI).

**Files.**
- `web/src/codec/audio_encoder.js` — already drafted in S4, now wire its
  output to `transport.sendAudio()`.
- `web_receiver.exe` — add the audio channel decode path, mirroring the
  block in `e2e_pipeline_test.cpp:406-512`.
- E2E test gains an `--audio` switch and an audio-packet count assertion
  (loose threshold — Chrome under headless may produce no audio if no
  source is playing, in which case skip rather than fail).

**Acceptance.** With audio playing on the host, the test asserts
`audio_packets >= 0.95 * expected`. With `--no-audio`, asserts the audio
channel was never opened.

**Test.** E2E gets a second test case parameterized on `--audio`.

### S11 — Stats endpoint

**Goal.** Receiver exposes `/api/stats` so the test harness can poll
without parsing log lines.

**Files.**
- `src/signaling/include/lumen/signaling/signaling_server.h` — add
  `RegisterStatsProvider(std::function<json()>)`.
- `web_receiver.exe` — register a stats provider that returns
  `received_video_frames`, `decoded_frames`, `decode_errors`,
  `latency_p50_us`, `latency_p95_us`, `latency_p99_us`.
- `tests/web_e2e_test.cpp` — `WaitForFinalStats()` polls
  `GET /api/stats` instead of scraping logs.

**Acceptance.** `curl http://localhost:18080/api/stats` returns valid
JSON during a run.

**Test.** Harness now uses the endpoint.

---

## 5. Build system changes

### 5.1 CMake

- `CMakeLists.txt` — no change.
- `src/CMakeLists.txt` — `add_subdirectory(signaling)`.
- `platform/windows/CMakeLists.txt` — `add_subdirectory(apps/web_receiver)`.
- `tests/CMakeLists.txt`:
  ```cmake
  if(WIN32)
    add_executable(web_e2e_test web_e2e_test.cpp helpers/subprocess.cpp)
    target_link_libraries(web_e2e_test PRIVATE
      lumen_platform_windows lumen_signaling httplib::httplib)
    add_dependencies(web_e2e_test web_receiver)

    add_custom_target(web_dist
      COMMAND npm --prefix ${CMAKE_SOURCE_DIR}/web run build
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Building web sender bundle")
    add_dependencies(web_e2e_test web_dist)
  endif()
  ```

### 5.2 vcpkg

`vcpkg.json` adds:
```json
{
  "dependencies": ["opus", "msquic", "cpp-httplib"]
}
```

### 5.3 Web

- `web/package.json` — `vite`, `vitest`. No framework. ESM only.
- `web/vite.config.js` — `build.outDir: 'dist'`, `build.target: 'es2022'`.
- `npm install` is a one-time developer step (documented in README).

---

## 6. Testing strategy

| Step | Test type | Where |
|---|---|---|
| S1 | Unit (vitest) + golden bytes | `web/src/wire/packet_header.test.js` |
| S2 | Build | — |
| S3 | Integration (httplib::Client) | `tests/signaling_server_test.cpp` |
| S4 | Manual + Playwright headed | `web/test/encoder_smoke.spec.js` (later) |
| S5 | In-process integration | `tests/ws_transport_test.cpp` |
| S6 | Manual hand-run | — |
| S7 | Manual hand-run | — |
| S8 | E2E subprocess | `tests/web_e2e_test.cpp` |
| S9 | E2E subprocess (TLS variant) | `tests/web_e2e_test.cpp` |
| S10 | E2E subprocess (audio variant) | `tests/web_e2e_test.cpp` |
| S11 | E2E subprocess polling stats | `tests/web_e2e_test.cpp` |

**No mock for `WsTransportServer`.** It's tested end-to-end against a
loopback client. The cost of a mock isn't justified for a single backend
that only Phase 1 exists for.

---

## 7. Risks and mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Annex B vs AVCC mismatch with `MfVideoDecoder` | Medium | Verify on S6 before completing the slice. WebCodecs supports both via `avc.format`. |
| Headless Chrome won't auto-pick a screen | Medium | The `--auto-select-desktop-capture-source` flag is supported but documented as best-effort. Fallback: `--use-fake-device-for-media-stream` with a synthetic source for CI; real source for local runs. |
| Self-signed cert blocks WS in headless | Low | Use the SPKI hash flag pattern; tested in S9. |
| msquic dev cert pattern (Schannel) doesn't transfer to plain TCP+TLS | Medium | If Schannel for sockets is too painful, fall back to embedded Mbed TLS or BoringSSL in `lumen_platform_windows` for Phase 1. Decision deferred to S9. |
| HW encoder unavailable on CI runners | High | E2E test detects software fallback and exits with skip code 77 rather than failing. Local dev runs are the source of truth. |
| `MediaStreamTrackProcessor` worker transfer cost | Low | Measured early in S4; if it dominates, fall back to main-thread pumping with `requestVideoFrameCallback`. |
| WS frame codec bugs | Medium | Golden-vector tests in S5 cover the RFC 6455 cases we exercise (binary, ping, close, masking client→server). No fragmentation handling needed (we send whole messages). |

---

## 8. Sizing estimate

Rough order-of-magnitude only.

| Step | LoC | Effort |
|---|---|---|
| S0 | ~50 | 0.5 day |
| S1 | ~120 (incl. test) | 0.5 day |
| S2 | ~10 | 0.5 day |
| S3 | ~250 | 1 day |
| S4 | ~250 | 1.5 days |
| S5 | ~700 | 3 days |
| S6 | ~300 | 1.5 days |
| S7 | ~250 | 1 day |
| S8 | ~400 | 2 days |
| S9 | ~250 | 2 days |
| S10 | ~150 | 1 day |
| S11 | ~100 | 0.5 day |
| **Total** | **~2800** | **~15 days** |

Phase 2 (WebTransport) is roughly the same order of magnitude as S5 + S9
combined, plus the `WebTransport` JS wrapper.

---

## 9. Definition of done — Phase 1

- A developer on Windows can run `web_receiver.exe`, open the printed URL
  in Chrome, click Start (or auto-start on load), and see their screen +
  hear their system audio in the receiver window.
- `ctest -R web_e2e` passes from a clean checkout (Chrome installed).
- The web sender uses the same `MediaPacketHeader` byte layout as the
  native sender, validated by a golden-bytes test.
- The WS server accepts, upgrades, and dispatches per-channel binary
  messages with no fragmentation, mirroring the existing TCP backend's
  framing semantics.
- Signaling is one HTTP endpoint. No second process. No external broker.
- Phase 2 (WebTransport) requires no rework of S1–S4, S6–S8, or S10–S11.

---

## 10. Changes to defer to Phase 2

- Datagram audio path. Phase 1 audio is reliable over WS.
- Multi-stream video (keyframe-on-priority-stream). Phase 1 sends one
  channel.
- Adaptive bitrate driven by transport stats. Phase 1 uses fixed CBR.
- Connection migration. WS doesn't support it; WebTransport does.
- HEVC and AV1 codec selection. Phase 1 is H.264 only.
