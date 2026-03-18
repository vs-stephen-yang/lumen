# Transport Layer Research & Design

## 1. Overview

This document covers the transport layer design for Lumen — a cross-platform, real-time screen sharing pipeline. The transport sits between the encoder output and decoder input, carrying encoded video (H.264/HEVC/AV1) and audio (Opus) packets between peers.

### Requirements

| Requirement | Detail |
|---|---|
| **Connectivity** | Peer-to-peer (direct) with relay (TURN) fallback |
| **Protocol backends** | TCP (fully reliable), QUIC streams (reliable), QUIC datagrams (partially reliable), WebTransport |
| **Swappable** | Abstract C++ interface; backends selected at runtime |
| **Cross-platform** | Windows, macOS, iOS, Android |
| **Framing** | RTP-like media packet headers over the transport |
| **Feedback** | Congestion stats exposed to encoder for adaptive bitrate |

### Data the Transport Must Carry

```cpp
// Video: 100KB–3MB per frame (keyframes much larger than delta frames)
struct EncodedPacket {
    std::vector<uint8_t> data;
    FrameMetadata metadata;  // frame_index, capture_time_us, is_keyframe, etc.
};

// Audio: ~20–40 bytes per 20ms frame
struct EncodedAudioPacket {
    std::vector<uint8_t> data;
    Timestamp timestamp_us;
    uint32_t duration_us;
};
```

---

## 2. QUIC / WebTransport Library Evaluation

### 2.1 Comparison Matrix

| Library | Language | License | Windows | macOS | iOS | Android | QUIC Datagrams | WebTransport | Build System | Dependency Weight |
|---|---|---|---|---|---|---|---|---|---|---|
| **msquic** | C | MIT | First-class | Supported | Experimental | Experimental (Linux-based) | Yes | No (needs HTTP/3 layer) | CMake | Light (Schannel/OpenSSL) |
| **ngtcp2** | C | MIT | Yes | Yes | Yes (portable C) | Yes (portable C) | Yes | No (needs nghttp3) | CMake | Minimal (BYO TLS) |
| **quiche** | Rust + C FFI | BSD-2 | Yes | Yes | Yes | Yes | Yes | Yes (via HTTP/3) | Cargo + CMake | Medium (Rust toolchain, BoringSSL bundled) |
| **lsquic** | C | MIT | Yes | Yes | Unclear | Unclear | Yes | Partial | CMake | Medium (BoringSSL) |
| **mvfst** | C++ | MIT | Limited | Yes | No | No | Yes | No | Buck/CMake | **Very heavy** (folly, fizz, boost) |
| **picoquic** | C | MIT | Yes | Yes | Untested | Untested | Yes | Experimental | CMake | Light (picotls) |
| **Chromium QUIC** | C++ | BSD-3 | Yes | Yes | Yes | Yes | Yes | Yes (Chrome's impl) | gn/ninja | **Extremely heavy** (Chromium infra) |
| **wtransport** | Rust | MIT | Yes | Yes | Yes | Yes | Yes | Yes | Cargo | Medium (Rust toolchain) |

### 2.2 Detailed Evaluation

#### msquic (Microsoft)
**Pros:**
- First-class Windows support; ships in Windows kernel, .NET, IIS, Xbox
- Clean C API, easy to wrap with RAII C++ classes
- Active development, strong Microsoft backing
- Excellent performance — optimized for Windows I/O (IOCP, kernel bypass)
- vcpkg package available (`vcpkg install ms-quic`)
- QUIC datagram support (RFC 9221) for unreliable media delivery
- Used by game engines (Microsoft GDK) — real-time media pedigree

**Cons:**
- iOS support is experimental/community-contributed, not officially supported
- Android support works (Linux-based) but is not first-class
- No WebTransport — would need to build HTTP/3 framing on top or use a separate library
- C API requires manual RAII wrappers for C++ ergonomics

**Best for:** Windows-first deployments, game-style low-latency networking

#### ngtcp2
**Pros:**
- Pure C, compiles everywhere with zero platform-specific code
- Smallest dependency footprint of any option — just needs a TLS backend
- Maximum control over pacing, scheduling, and memory allocation
- CMake build system integrates cleanly with Lumen's existing build
- Used by curl as a QUIC backend — proven in production
- Pairs with nghttp3 for HTTP/3 (and thus WebTransport)
- BYO TLS: use BoringSSL (best for mobile), wolfSSL, or OpenSSL

**Cons:**
- Lower-level API: you must implement the event loop, timer management, and I/O yourself
- No built-in WebTransport (requires nghttp3 integration)
- Smaller community than msquic
- More work to reach a production-ready transport (but maximum control)

**Best for:** Cross-platform mobile targets, lean builds, maximum tunability

#### quiche (Cloudflare)
**Pros:**
- Excellent feature completeness: QUIC + HTTP/3 + WebTransport in one package
- Battle-tested at Cloudflare scale
- Used by curl and Android's DNS-over-QUIC
- Clean C FFI for integration from C++
- BoringSSL bundled — no TLS backend decisions needed

**Cons:**
- **Requires Rust toolchain** — adds cargo to the build pipeline, cross-compilation for iOS/Android is non-trivial
- C FFI requires manual memory management wrappers
- No native C++ API
- Build complexity: Rust + C cross-compilation matrix across 4 platforms

**Best for:** Projects that already use Rust, or where WebTransport is day-one critical

#### lsquic (LiteSpeed)
**Pros:**
- Mature, production-proven (LiteSpeed web server)
- HTTP/3 support built-in
- Clean callback-driven C API

**Cons:**
- Server-oriented design — less proven for client-side use
- Requires BoringSSL specifically (not OpenSSL)
- Mobile platform support is unclear
- Smaller community outside the web server ecosystem

**Verdict:** Not the best fit for a client-side media pipeline.

#### mvfst (Meta)
**Pros:**
- Native C++ API, clean design
- Production-proven at Meta scale

**Cons:**
- **Disqualifying dependency chain**: folly, fizz, fmt, glog, boost, double-conversion
- Windows and mobile support is limited
- Buck build system (CMake support is secondary)

**Verdict:** Reject — dependency chain is incompatible with a lean cross-platform project.

#### Chromium QUIC (libquiche / Google)
**Pros:**
- The most mature QUIC + WebTransport implementation (Chrome ships it)
- Full platform support (everywhere Chrome runs)

**Cons:**
- **Tightly coupled to Chromium's build system** (gn/ninja)
- Extracting it as a standalone library is a major engineering effort
- Dependencies on abseil, BoringSSL, and various Chromium infrastructure

**Verdict:** Reject for standalone use — extraction cost is prohibitive.

#### picoquic
**Pros:**
- Written by Christian Huitema (former QUIC WG chair) — deep protocol expertise
- Experimental WebTransport support
- Very small footprint

**Cons:**
- Research-oriented, smaller community
- Lower production confidence
- iOS/Android untested

**Verdict:** Good for experimentation, not for production.

### 2.3 Recommendation: Tiered Backend Strategy

| Platform | Primary Library | Rationale |
|---|---|---|
| **Windows** | **msquic** | First-class support, best perf, vcpkg integration |
| **macOS** | **ngtcp2** + BoringSSL | Portable C, minimal deps, easy to build |
| **iOS** | **ngtcp2** + BoringSSL | Same — pure C compiles for iOS without issues |
| **Android** | **ngtcp2** + BoringSSL | Same — NDK-friendly, no platform-specific code |
| **All (fallback)** | **TCP** | Always available, zero dependencies |
| **WebTransport** | **ngtcp2 + nghttp3** | Adds HTTP/3 framing for browser interop |

The abstract interface means switching backends is a configuration choice, not an architectural change. Start with msquic on Windows, add ngtcp2 for other platforms, TCP for universal fallback.

---

## 3. Industry Practice & Trends

### How Production Systems Handle Transport

| System | Transport | Key Design Choices |
|---|---|---|
| **WebRTC** | DTLS-SRTP over ICE/UDP | Unreliable datagrams for media, SCTP data channels for control. ICE/STUN/TURN for NAT traversal. GCC or SendSide BWE for congestion. |
| **Zoom** | Custom UDP protocol | Selective retransmission (retransmit if time permits, drop if stale). Relay-first architecture. FEC before retransmission. |
| **Discord** | WebRTC (modified) | Media over unreliable UDP, signaling over WebSocket. Custom congestion control. |
| **OBS WHIP/WHEP** | WebRTC | HTTP-based signaling, standard WebRTC media transport for ingest/playback. |
| **LiveKit** | WebRTC (SFU) | Selective forwarding. Exploring QUIC for server-to-server. Client-facing is WebRTC. |

### Media over QUIC (MoQ) — The Emerging Standard

The IETF Media over QUIC (MoQ) working group is standardizing real-time media delivery over QUIC/WebTransport. Key facts:

- **Status**: Active IETF draft; finalized RFC expected in 2026
- **Backers**: Cloudflare, Akamai, Cisco, YouTube, Red5
- **Architecture**: Publisher → relay network → subscriber (pub/sub model)
- **Latency**: Sub-250ms target
- **Key advantage**: Combines WebRTC's low latency with HLS/DASH's scalability
- **Cloudflare** has launched MoQ relay infrastructure across 330+ cities

**Relevance to Lumen**: MoQ's architecture (QUIC streams for reliable data, datagrams for latency-sensitive media, independent streams to avoid HoL blocking) validates Lumen's proposed channel design. However, MoQ is a pub/sub distribution protocol — Lumen's 1:1 screen sharing use case is simpler and doesn't need the full MoQ relay infrastructure. The framing ideas and reliability patterns are directly applicable.

### Key Takeaway

The industry is moving from WebRTC's DTLS-SRTP toward QUIC-based media transport. Lumen's design aligns with this trajectory: QUIC streams for reliable data (signaling, keyframes), QUIC datagrams for latency-sensitive media (delta frames, audio), and built-in congestion feedback for adaptive bitrate.

---

## 4. Abstract Interface Design

### 4.1 Transport Types

```cpp
// src/transport/include/lumen/transport/transport_types.h

namespace lumen {

/// Identifies what kind of data a channel carries.
enum class ChannelType {
    kVideoReliable,       // Keyframes, codec config (QUIC stream)
    kVideoUnreliable,     // Delta frames — droppable (QUIC datagram)
    kAudioReliable,       // Audio frames, reliable delivery (QUIC stream)
    kAudioUnreliable,     // Audio frames, low-latency (QUIC datagram)
    kControl,             // Signaling, stats, keyframe requests (QUIC stream)
};

/// Reliability mode for a channel.
enum class ReliabilityMode {
    kReliableOrdered,     // TCP-like: in-order, guaranteed delivery
    kReliableUnordered,   // Guaranteed delivery, may arrive out of order
    kUnreliable,          // Best-effort datagram, may be lost
    kPartiallyReliable,   // Retransmit within time budget, then drop
};

/// Priority levels for send scheduling.
enum class SendPriority {
    kControl = 0,      // Highest
    kAudio = 1,
    kVideoKey = 2,
    kVideoDelta = 3,
    kBulk = 4,         // Lowest
};

/// Transport-level configuration.
struct TransportConfig {
    std::string remote_address;
    uint16_t remote_port = 0;

    // TLS
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = true;
    std::vector<std::string> alpn;       // e.g., {"lumen/1"}

    // Tuning
    uint32_t max_datagram_size = 1200;   // Conservative MTU
    uint32_t idle_timeout_ms = 30000;
    uint32_t initial_rtt_ms = 50;
    bool enable_migration = false;       // QUIC connection migration

    // NAT traversal
    std::string stun_server;
    std::string turn_server;
    std::string turn_username;
    std::string turn_credential;
};

/// Connection state.
enum class ConnectionState {
    kDisconnected,
    kConnecting,
    kConnected,
    kDraining,
    kFailed,
};

/// Congestion and path stats — exposed to encoder for adaptive bitrate.
struct TransportStats {
    uint32_t rtt_us = 0;
    uint32_t rtt_variance_us = 0;
    uint32_t min_rtt_us = 0;

    uint64_t estimated_bandwidth_bps = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t bytes_in_flight = 0;

    double packet_loss_rate = 0.0;       // 0.0 – 1.0
    uint64_t packets_lost = 0;
    uint64_t packets_sent = 0;

    uint64_t congestion_window_bytes = 0;
    bool is_congested = false;

    uint64_t datagrams_sent = 0;
    uint64_t datagrams_lost = 0;
    uint64_t datagrams_received = 0;
};

/// RTP-like media packet header (simplified for 1:1 screen sharing).
struct MediaPacketHeader {
    uint8_t version = 1;
    uint8_t payload_type;            // Maps to ChannelType
    uint16_t sequence_number = 0;
    uint32_t timestamp_us_low32 = 0; // Low 32 bits of capture timestamp
    uint32_t ssrc = 0;              // Stream identifier

    // Fragmentation (for large keyframes)
    uint16_t fragment_index = 0;
    uint16_t fragment_count = 1;
    uint32_t frame_index = 0;

    bool is_keyframe = false;
    bool is_last_fragment = false;

    static constexpr size_t kSerializedSize = 24;

    void Serialize(uint8_t* buf) const;
    static MediaPacketHeader Deserialize(const uint8_t* buf);
};

using ConnectionStateCallback = std::function<void(ConnectionState)>;
using DataReceivedCallback = std::function<void(ChannelType, const uint8_t*, size_t)>;
using StatsCallback = std::function<void(const TransportStats&)>;

}  // namespace lumen
```

### 4.2 TransportChannel (abstract)

```cpp
// src/transport/include/lumen/transport/transport_channel.h

namespace lumen {

/// A logical channel within a connection.
/// Maps to a QUIC stream (reliable) or datagram flow (unreliable).
class TransportChannel {
public:
    virtual ~TransportChannel() = default;

    /// Send data. Large payloads are automatically fragmented.
    virtual Result<void> Send(const uint8_t* data, size_t size,
                               const FrameMetadata& metadata) = 0;

    /// Set callback for received (reassembled) frames.
    virtual void SetReceiveCallback(DataReceivedCallback callback) = 0;

    virtual ChannelType GetType() const = 0;
    virtual ReliabilityMode GetReliability() const = 0;
    virtual void SetPriority(SendPriority priority) = 0;
    virtual size_t GetMaxPayloadSize() const = 0;
};

}  // namespace lumen
```

### 4.3 TransportConnection (abstract)

```cpp
// src/transport/include/lumen/transport/transport_connection.h

namespace lumen {

/// A connection between two peers, containing multiple channels.
class TransportConnection {
public:
    virtual ~TransportConnection() = default;

    virtual TransportChannel* GetChannel(ChannelType type) = 0;
    virtual ConnectionState GetState() const = 0;
    virtual void SetStateCallback(ConnectionStateCallback callback) = 0;

    /// Get transport stats (RTT, bandwidth, loss) for adaptive bitrate.
    virtual TransportStats GetStats() const = 0;
    virtual void SetStatsCallback(StatsCallback callback,
                                   uint32_t interval_ms = 1000) = 0;

    /// Request peer to send a keyframe (sent over control channel).
    virtual Result<void> RequestKeyframe() = 0;

    /// Send/receive arbitrary control messages.
    virtual Result<void> SendControlMessage(const uint8_t* data, size_t size) = 0;
    virtual void SetControlMessageCallback(
        std::function<void(const uint8_t*, size_t)> callback) = 0;

    virtual Result<void> Close() = 0;
    virtual std::string GetRemoteAddress() const = 0;
};

}  // namespace lumen
```

### 4.4 TransportClient / TransportServer (abstract factories)

```cpp
// src/transport/include/lumen/transport/transport_client.h

namespace lumen {

class TransportClient {
public:
    virtual ~TransportClient() = default;
    virtual Result<void> Initialize(const TransportConfig& config) = 0;
    virtual Result<std::unique_ptr<TransportConnection>> Connect() = 0;
    virtual void Shutdown() = 0;
};

}  // namespace lumen
```

```cpp
// src/transport/include/lumen/transport/transport_server.h

namespace lumen {

using IncomingConnectionCallback =
    std::function<void(std::unique_ptr<TransportConnection>)>;

class TransportServer {
public:
    virtual ~TransportServer() = default;
    virtual Result<void> Initialize(const TransportConfig& config) = 0;
    virtual Result<void> Start(IncomingConnectionCallback callback) = 0;
    virtual Result<void> Stop() = 0;
    virtual void Shutdown() = 0;
    virtual std::string GetListenAddress() const = 0;
    virtual uint16_t GetListenPort() const = 0;
};

}  // namespace lumen
```

---

## 5. RTP Framing Strategy

### Why Not Full RTP?

Full RTP (RFC 3550) with RTCP was designed to multiplex many participants over bare UDP. It provides:
- SSRC collision handling (not needed for 1:1)
- RTCP sender/receiver reports (QUIC provides better congestion feedback)
- DTLS-SRTP encryption (QUIC already encrypts everything)
- Jitter estimation via RTCP (QUIC provides RTT natively)

For a 1:1 screen sharing pipeline over QUIC, most of RTP's machinery is redundant.

### Lumen's Approach: MediaPacketHeader

A simplified 24-byte header carrying only what's needed:

| Field | Size | Purpose |
|---|---|---|
| `version` | 1B | Protocol version |
| `payload_type` | 1B | Channel/codec identifier |
| `sequence_number` | 2B | Per-channel loss detection |
| `timestamp_us_low32` | 4B | A/V sync, jitter buffer playout |
| `ssrc` | 4B | Stream identifier |
| `fragment_index` | 2B | Fragment position in frame |
| `fragment_count` | 2B | Total fragments for frame |
| `frame_index` | 4B | Frame sequence number |
| `flags` | 4B | `is_keyframe`, `is_last_fragment`, reserved |

### Fragmentation

Large frames (especially keyframes at 100KB–3MB) must be fragmented into MTU-sized packets (~1200 bytes for QUIC datagrams):

- A 500KB keyframe → ~420 fragments
- `FrameFragmenter` splits frame + header into MTU-sized packets
- `FrameReassembler` collects fragments and delivers complete frames
- **Keyframes are sent on reliable QUIC streams** (all fragments guaranteed to arrive)
- **Delta frames are sent as QUIC datagrams** (fragments may be lost; incomplete frames are discarded)

### Future RTP Interop

If interoperability with WebRTC or other RTP-based systems is needed, a `RtpFramer` adapter can map between `MediaPacketHeader` and proper RTP/RTCP. The abstract interface makes this a new backend, not an architectural change.

---

## 6. Challenges & Risks

### 6.1 NAT Traversal (P2P)

**Challenge:** Most peers are behind NAT. Direct UDP connections require hole-punching.

**Approach:**
1. **STUN binding** — both peers discover their public IP:port
2. **Candidate exchange** — peers exchange addresses via signaling (WebSocket/HTTP)
3. **Connectivity checks** — try each candidate pair; first successful QUIC handshake wins
4. **TURN fallback** — if all direct paths fail, relay through TURN server

**Recommendation:** Use the `juice` library (BSD license, lightweight ICE in C) rather than implementing ICE from scratch. It handles STUN/TURN/candidate gathering and integrates cleanly with C++ projects.

**Risk:** ICE adds 1–3 seconds to connection setup. Mitigate with TURN pre-allocation for fastest fallback.

### 6.2 Keyframe Burst Handling

**Challenge:** A 4K HEVC keyframe can reach 1–3MB. Bursting this data causes congestion spikes and packet loss.

**Mitigation strategy:**
- Send keyframes on a **reliable QUIC stream** (guaranteed delivery, paced by congestion control)
- Use the encoder's **intra refresh** mode to spread keyframe data across multiple frames
- Set `max_slice_size_bytes` on the encoder to produce smaller NAL units
- Transport-level **pacing**: spread fragments across the congestion window, don't burst

### 6.3 Head-of-Line (HoL) Blocking

**Challenge:** TCP has a single byte stream — one lost packet blocks everything.

**How QUIC solves it:**
- `kVideoReliable` = one QUIC stream (keyframes don't block audio)
- `kVideoUnreliable` = QUIC datagrams (no blocking at all)
- `kAudioReliable` = separate QUIC stream (independent of video retransmissions)
- `kControl` = separate QUIC stream

**For TCP fallback:** Application-level multiplexing with `[channel_id | length | payload]` framing and a priority send queue (audio/control preempt video).

### 6.4 Partial Reliability (Stale Frame Dropping)

**Challenge:** A delta frame arriving after the next frame has been rendered is useless. Retransmitting it wastes bandwidth.

**Approach:**
- Delta frames sent as QUIC datagrams (inherently unreliable)
- **Staleness deadline**: `capture_time + (1/fps) * 1.5` — unsent fragments are dropped after this
- Receiver-side reassembler discards incomplete frames after 2x frame interval
- Loss reported to jitter buffer, which signals PLC (audio) or freeze/skip (video)

### 6.5 A/V Synchronization

**Challenge:** Audio and video travel on separate channels with different latencies.

**Approach:**
- Both packet types carry `capture_time_us` from the **same sender clock**
- `MediaPacketHeader` preserves this timestamp through transport
- Jitter buffer uses **capture timestamps** for playout alignment (not arrival times)
- `TransportStats::rtt_us` helps the jitter buffer estimate one-way delay

### 6.6 Congestion Control → Adaptive Bitrate

**Challenge:** The video encoder must adapt to available bandwidth in real-time.

**Feedback loop:**
```
TransportStats (bandwidth, loss, RTT)
        ↓
BitrateController (smoothing, clamping)
        ↓
VideoEncoder::SetBitrate() / SetFrameRate()
```

- QUIC's built-in congestion controller (Cubic or BBR) provides `estimated_bandwidth_bps`
- Reserve 15% of bandwidth for audio + control overhead
- Exponential moving average smoothing to prevent oscillation
- Drop FPS under severe congestion (60 → 30)
- Ramp-up phase: start conservative, probe upward

### 6.7 Connection Migration (Mobile)

**Challenge:** Mobile devices switch between Wi-Fi and cellular.

**Approach:** QUIC connection migration (supported by msquic natively). The connection survives network changes transparently. Enabled via `TransportConfig::enable_migration`.

### 6.8 Security

**Approach:** QUIC mandates TLS 1.3 — all media is encrypted in transit. No additional SRTP/DTLS layer needed (unlike WebRTC).
- Server holds a TLS certificate (self-signed for LAN, CA-signed for internet)
- Client verifies server certificate (`TransportConfig::verify_peer`)
- 0-RTT resumption for fast reconnection (with anti-replay protection)

---

## 7. Phased Implementation Plan

### Phase 1: Abstract Interfaces + TCP Backend

**Goal:** End-to-end data flow with the simplest transport.

**Files to create:**
```
src/transport/
    include/lumen/transport/
        transport_types.h
        transport_channel.h
        transport_connection.h
        transport_client.h
        transport_server.h
        media_packet_header.h
    src/
        media_packet_header.cpp
        frame_fragmenter.h / .cpp
        frame_reassembler.h / .cpp

platform/windows/transport/
    tcp_transport_client.h / .cpp
    tcp_transport_server.h / .cpp
    tcp_transport_connection.h / .cpp

tests/
    mocks/mock_transport_connection.h
    mocks/mock_transport_channel.h
    transport_tcp_test.cpp
```

**Files to modify:**
- `src/common/include/lumen/common/error.h` — add transport error codes
- `src/transport/CMakeLists.txt` — add sources and targets
- `vcpkg.json` — no new deps for TCP

TCP multiplexing: `[channel_id: u8][length: u32][payload]` with priority send queue.

### Phase 2: QUIC Backend (msquic on Windows)

**Goal:** Replace HoL-blocking TCP with QUIC streams + datagrams.

**Files to create:**
```
platform/windows/transport/
    quic_transport_client.h / .cpp
    quic_transport_server.h / .cpp
    quic_transport_connection.h / .cpp
    quic_stream_channel.h / .cpp
    quic_datagram_channel.h / .cpp

tests/
    transport_quic_test.cpp
```

**Files to modify:**
- `vcpkg.json` — add `ms-quic` dependency
- `platform/windows/CMakeLists.txt` — link msquic

### Phase 3: Jitter Buffer

**Goal:** Smooth playback timing on the receive side.

**Files to create:**
```
src/jitter_buffer/
    include/lumen/jitter_buffer/
        jitter_buffer.h
        video_jitter_buffer.h
        audio_jitter_buffer.h
    src/
        video_jitter_buffer.cpp
        audio_jitter_buffer.cpp

tests/
    jitter_buffer_test.cpp
```

### Phase 4: Adaptive Bitrate Controller

**Goal:** Close the congestion → encoder feedback loop.

**Files to create:**
```
src/transport/
    include/lumen/transport/bitrate_controller.h
    src/bitrate_controller.cpp

tests/
    bitrate_controller_test.cpp
```

### Phase 5: NAT Traversal / P2P

**Goal:** Enable connections without pre-known IP addresses.

**Files to create:**
```
src/transport/
    include/lumen/transport/ice_agent.h
    src/ice_agent.cpp
    src/stun_client.h / .cpp
```

Consider integrating `juice` (LGPL-2.1/MIT dual-license, lightweight ICE library in C).

### Phase 6: Cross-Platform QUIC (ngtcp2)

**Goal:** QUIC on macOS, iOS, Android.

**Files to create:**
```
platform/macos/transport/    — ngtcp2-based backend
platform/ios/transport/      — same ngtcp2 backend
platform/android/transport/  — ngtcp2 backend + JNI
```

**Modify:** `vcpkg.json` — add `ngtcp2`, `boringssl`

### Phase 7: WebTransport (Optional)

**Goal:** Enable browser-based receivers.

Add HTTP/3 framing via nghttp3 on top of ngtcp2. Enables a web viewer without native client installation.

---

## 8. Directory Structure (Final)

```
src/transport/
    include/lumen/transport/
        transport_types.h           // Enums, configs, stats, header
        transport_channel.h         // Abstract channel
        transport_connection.h      // Abstract connection
        transport_client.h          // Abstract client factory
        transport_server.h          // Abstract server factory
        media_packet_header.h       // RTP-like framing
        bitrate_controller.h        // Adaptive bitrate logic
    src/
        media_packet_header.cpp
        frame_fragmenter.h / .cpp
        frame_reassembler.h / .cpp
        bitrate_controller.cpp

src/jitter_buffer/
    include/lumen/jitter_buffer/
        jitter_buffer.h
        video_jitter_buffer.h
        audio_jitter_buffer.h
    src/
        video_jitter_buffer.cpp
        audio_jitter_buffer.cpp

platform/windows/transport/
    tcp_transport_*.h/.cpp          // TCP fallback
    quic_transport_*.h/.cpp         // msquic backend
    quic_stream_channel.h/.cpp
    quic_datagram_channel.h/.cpp

platform/{macos,ios,android}/transport/
    quic_transport_*.h/.cpp         // ngtcp2 backend
```

---

## Sources

- [msquic — GitHub](https://github.com/microsoft/msquic)
- [msquic Release & Platform Support](https://microsoft.github.io/msquic/msquicdocs/docs/Release.html)
- [ngtcp2 — GitHub](https://github.com/ngtcp2/ngtcp2)
- [quiche (Cloudflare) — GitHub](https://best-of-web.builder.io/library/cloudflare/quiche)
- [IETF Media over QUIC Working Group](https://datatracker.ietf.org/group/moq/about/)
- [MoQ Transport Draft](https://moq-wg.github.io/moq-transport/draft-ietf-moq-transport.html)
- [Cloudflare MoQ Blog](https://blog.cloudflare.com/moq/)
- [MOQ Protocol Explained — WebRTC.ventures](https://webrtc.ventures/2025/10/moq-protocol-explained-unifying-real-time-and-scalable-streaming/)
- [HTTP/3 Open Source Support Analysis](https://httptoolkit.com/blog/http3-quic-open-source-support-nowhere/)
- [MsQuic for Game Development — Microsoft](https://learn.microsoft.com/en-us/gaming/gdk/docs/features/console/networking/game-mesh/msquic-intro-networking)
