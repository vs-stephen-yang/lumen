# Implementation Plan: Windows Audio Decoding & Playback (WASAPI Renderer)

## Context

Lumen's audio **capture and encoding** pipeline is complete on Windows (WASAPI loopback → Opus encode → Opus decode, all tested). The **playback/rendering** side is entirely absent. This plan implements a WASAPI shared-mode audio renderer that takes decoded PCM audio from `OpusAudioDecoder` and plays it through the system audio endpoint. This is the receiver side of the audio pipeline.

## Challenges & Complexities

### 1. Ring Buffer Between Decoder and Renderer

**Problem:** The Opus decoder produces `DecodedAudioFrame` objects at irregular, network-driven intervals (bursts or gaps). WASAPI fires its buffer-ready event on a strict ~10ms schedule and demands exactly the number of samples it requests — no more, no fewer. These two timing domains must be decoupled.

**Complexity:**
- Opus frames are 960 samples/channel (20ms). WASAPI may request arbitrary counts (e.g., 480 samples for a 10ms period). The buffer must operate at the sample level, splitting decoder frames across WASAPI callbacks.
- Thread safety: decoder thread writes, WASAPI render thread reads — concurrent access.
- Sizing trade-off: too small (< 40ms) → frequent underruns; too large → added latency.

**Approach:** Lock-free SPSC (single-producer, single-consumer) ring buffer of interleaved float32 samples. Power-of-two capacity (16384 floats ≈ 170ms at 48kHz stereo). Pre-buffer 40ms (2 Opus frames) before starting WASAPI playback.

**Status:** Solve now — fundamental to the renderer.

---

### 2. Underrun Handling (Decoder Too Slow / Network Loss)

**Problem:** WASAPI's event fires, but the ring buffer has fewer samples than requested (or is empty). WASAPI will not wait — the buffer must be filled or silence occurs.

**Complexity:**
- Opus PLC (`DecodePLC()`) could generate replacement audio, but invoking it from the render thread is problematic: the decoder is not thread-safe, and cross-thread decoder calls create tight coupling.
- PLC properly belongs in the jitter buffer (not yet built), which detects missing packets on the decoder thread and pre-inserts concealment frames.

**Approach:** Output silence on underrun (zero-fill the remainder). Track `underrun_count_` and `total_underrun_samples_` as atomic counters exposed via `GetStats()`. PLC integration deferred to jitter buffer.

**Status:** Silence-on-underrun now. PLC deferred.

---

### 3. Overrun Handling (Decoder Too Fast / Bursty Arrival)

**Problem:** Decoder produces frames faster than the renderer consumes them. The ring buffer fills up.

**Complexity:**
- Drop oldest (advance read pointer) keeps freshest audio but violates SPSC invariant (producer would move consumer's index).
- Drop newest (reject incoming frame) is simpler and safe.
- Blocking the decoder could back-pressure the network receive path.

**Approach:** Drop newest — if `ring_buffer.WritableCount() < frame.samples.size()`, discard the frame and increment `overrun_count_`. Rare in practice; indicates a clock or congestion problem the jitter buffer should handle.

**Status:** Drop-newest now. Adaptive response deferred.

---

### 4. Clock Drift

**Problem:** Sender captures at its hardware clock rate (nominally 48kHz). Receiver plays at its hardware clock rate (also nominally 48kHz). Crystal oscillator tolerances of ±50ppm cause ~2.4 samples/second drift. Over 10 minutes: ~30ms drift — enough to cause underrun or overrun.

**Complexity:**
- `IAudioClockAdjustment::SetSampleRate()` adjusts playback rate but has limited range and is Windows-only.
- Adaptive resampling (libspeexdsp) is cross-platform but adds CPU cost and implementation complexity.
- Sample skip/insert is simple but audible.

**Approach:** Defer active correction. Instrument now: track ring buffer fill level (EWMA) on each WASAPI callback, expose `buffer_level_ms` and drift rate via `GetStats()`. Initialize WASAPI with `AUDCLNT_STREAMFLAGS_RATEADJUST` so `IAudioClockAdjustment` is available later. The 100ms ring buffer provides ~20 minutes of runway at worst-case drift — sufficient for testing.

**Status:** Instrumentation now. Active correction deferred to jitter buffer.

---

### 5. Audio-Video Synchronization

**Problem:** Audio and video frames carry `timestamp_us` from the sender's clock. Without coordination, the two streams drift apart on the receiver.

**Complexity:**
- Standard approach: audio is the master clock, video adjusts to match (humans are more sensitive to audio glitches than video jitter).
- Requires the audio renderer to expose its current playback position so the video renderer can query it.
- No transport layer exists yet, so there's no cross-stream coordination point.

**Approach:** Expose `GetPlaybackTimestamp()` returning the `timestamp_us` of the audio currently being output. This is the foundation for future A/V sync. Actual sync logic deferred to transport/orchestrator.

**Status:** Expose timestamp now. Sync logic deferred.

---

### 6. Format Mismatch

**Problem:** Opus decodes to 48kHz stereo float32. The system mix format (from `GetMixFormat()`) is usually 48kHz float32 but could differ — different sample rate, channel count, or sample format.

**Complexity:**
- WASAPI capture side rejects non-48kHz with `kUnsupported`. The render side needs to be more forgiving since the user can't control the receiver's audio config.
- `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` inserts Windows Audio Engine resampler (~4ms added latency).
- Channel mismatch (stereo → mono or 5.1) requires up/down-mixing.

**Approach:** Initialize WASAPI with the Opus output format (48kHz float32 stereo) and `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`. Windows handles format conversion transparently. If `Initialize()` fails entirely, fall back to `GetMixFormat()` and reject if incompatible.

**Status:** Solve now.

---

### 7. Device Hot-Plug

**Problem:** User unplugs headphones mid-playback. The default audio endpoint changes. The WASAPI render client becomes invalid.

**Complexity:**
- Must detect, re-enumerate, and reinitialize on the new default device with minimal dropout.
- Ring buffer contents must be preserved — only the WASAPI output path is recycled.
- Brief audio dropout (10-50ms) during reinitialization is inherent and acceptable.

**Approach:** Replicate the `WasapiLoopbackCapture` pattern exactly — `IMMNotificationClient` inner class, `atomic<bool> device_changed_` flag, check on render thread before each iteration, stop/release/re-init/restart on change.

**Status:** Solve now.

---

### 8. Latency Budget

**Target render-side latency breakdown:**

| Component | Target | Notes |
|-----------|--------|-------|
| WASAPI shared-mode buffer | 10ms | System audio engine period |
| Ring buffer pre-fill | 40ms | 2 Opus frames before playback starts |
| Ring buffer steady-state fill | ~20ms | Average during normal operation |
| **Total render latency** | **~50ms** | Within lip-sync threshold (~80ms) |

**Complexity:** Pre-buffer too small → underruns on first hiccup. Pre-buffer too large → user-perceptible delay. Need configurable `pre_buffer_ms` (default 40ms, range 20-200ms).

**Status:** Define budget and make configurable now.

---

### 9. Event-Driven vs. Polling Thread Model

**Problem:** The render thread must feed WASAPI on a strict schedule.

**Complexity:**
- WASAPI loopback capture uses **polling** (10ms `WaitForSingleObject`) because loopback mode does NOT support `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`.
- WASAPI rendering DOES support event-driven mode — lower latency, more precise timing, lower CPU.
- The render thread must also respond to stop signals and device-change flags.

**Approach:** Event-driven render thread. `WaitForMultipleObjects({wasapi_event, stop_event})` — wakes on either WASAPI buffer-ready or shutdown signal. MMCSS "Audio" thread priority. COM initialized per-thread.

**Status:** Solve now.

---

### 10. Testing Without Transport (Feedback Loop Risk)

**Problem:** No transport layer exists. The test must run the full pipeline locally: capture → encode → decode → render. If the output device is the same as the loopback capture source, this creates a feedback loop (played audio is re-captured, re-encoded, re-decoded, re-played).

**Complexity:**
- Produces an echo/reverb effect (not oscillation, since Opus adds enough latency).
- Automated audio quality measurement (PESQ/POLQA) is impractical for system loopback.
- Must verify pipeline health via statistics, not audio content.

**Approach:** Run for 5 seconds with `PlaySound` generating system audio (matching existing test pattern). Verify via `GetStats()`: `frames_played > 0`, low `underrun_count`, `buffer_level_ms` in expected range. Print latency statistics. Document feedback loop as expected behavior.

**Status:** Solve now.

---

### Summary: Solve Now vs. Defer

| Challenge | Now | Defer To |
|-----------|-----|----------|
| Ring buffer (SPSC) | Yes | — |
| Underrun → silence | Yes | PLC (jitter buffer) |
| Overrun → drop newest | Yes | Adaptive (jitter buffer) |
| Clock drift | Instrumentation + RATEADJUST flag | Active correction (jitter buffer) |
| A/V sync | Expose playback timestamp | Sync coordinator (transport) |
| Format mismatch | AUTOCONVERTPCM | — |
| Device hot-plug | Yes | — |
| Latency budget | Configurable pre-buffer | Dynamic adaptation |

## Implementation Steps

### Step 1: Extend Common Types
**Modify:** `src/common/include/lumen/common/types.h`
- Add `AudioRendererConfig` struct: `sample_rate`, `channels`, `pre_buffer_ms` (default 40), `buffer_capacity_ms` (default 100)
- Add `AudioRendererStats` struct: `frames_played`, `underrun_count`, `overrun_count`, `total_underrun_samples`, `buffer_level_ms`

**Modify:** `src/common/include/lumen/common/error.h`
- Add `kAudioRendererError` to `ErrorCode`

### Step 2: Abstract AudioRenderer Interface
**Create:** `src/renderer/include/lumen/renderer/audio_renderer.h`
- `AudioRenderer` abstract class: `Initialize(config)`, `Start()`, `Stop()`, `QueueFrame(DecodedAudioFrame)`, `GetStats()`, `GetPlaybackTimestamp()`
- Push model — caller pushes decoded frames, renderer pulls from internal ring buffer on WASAPI schedule
- No `Present()` method (unlike VideoRenderer — audio presents automatically when WASAPI consumes it)

### Step 3: SPSC Ring Buffer
**Create:** `platform/windows/renderer/audio_ring_buffer.h`
- Header-only lock-free SPSC ring buffer of interleaved float32 samples
- Power-of-two capacity, atomic read/write indices
- Methods: `Write(float*, count)` → returns samples written, `Read(float*, count)` → returns samples read, `ReadableCount()`, `WritableCount()`, `Clear()`

### Step 4: WASAPI Audio Renderer
**Create:** `platform/windows/renderer/wasapi_audio_renderer.h`
**Create:** `platform/windows/renderer/wasapi_audio_renderer.cpp`

Key implementation:
- `IAudioClient::Initialize()` with `AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`
- Format: 48kHz stereo float32 (Opus output format, not system mix format)
- Event-driven render thread: `WaitForMultipleObjects({wasapi_event, stop_event})`
- On event: `GetCurrentPadding()` → compute writable frames → read from ring buffer → `GetBuffer()`/`ReleaseBuffer()`
- Pre-buffering: output silence until ring buffer reaches `pre_buffer_ms` threshold, then switch to real data
- Underrun: zero-fill + increment counter
- Overrun: `QueueFrame()` drops frame if ring buffer full + increment counter
- Device hot-plug: `IMMNotificationClient` (same pattern as WASAPI capture)
- MMCSS "Audio" thread priority
- Timestamp tracking: maintain mapping from ring buffer read position to `timestamp_us`
- `GetStats()`: return current buffer level, underrun/overrun counts, frames played
- `GetPlaybackTimestamp()`: return timestamp of audio currently being output

### Step 5: CMake Updates
**Modify:** `platform/windows/CMakeLists.txt`
- Add `renderer/wasapi_audio_renderer.cpp` to source list

### Step 6: Mock Implementation
**Create:** `tests/mocks/mock_audio_renderer.h`

### Step 7: Integration Test
**Create:** `tests/audio_roundtrip_test.cpp`
- Full pipeline: WASAPI Capture → Opus Encode → Opus Decode → WASAPI Render
- Play system sounds via `PlaySound` in background thread
- Run for 5 seconds, verify stats: `frames_played > 0`, low `underrun_count`, `buffer_level_ms` in range

**Modify:** `tests/CMakeLists.txt`
- Add `audio_roundtrip_test` target

## Files Summary

| Action | File |
|--------|------|
| Modify | `src/common/include/lumen/common/types.h` |
| Modify | `src/common/include/lumen/common/error.h` |
| Create | `src/renderer/include/lumen/renderer/audio_renderer.h` |
| Create | `platform/windows/renderer/audio_ring_buffer.h` |
| Create | `platform/windows/renderer/wasapi_audio_renderer.h` |
| Create | `platform/windows/renderer/wasapi_audio_renderer.cpp` |
| Modify | `platform/windows/CMakeLists.txt` |
| Create | `tests/mocks/mock_audio_renderer.h` |
| Create | `tests/audio_roundtrip_test.cpp` |
| Modify | `tests/CMakeLists.txt` |

## Critical Reference Files
- `src/renderer/include/lumen/renderer/video_renderer.h` — interface pattern to follow
- `platform/windows/renderer/d3d11_swapchain_renderer.h/.cpp` — platform renderer pattern
- `platform/windows/capture/wasapi_loopback_capture.cpp` — WASAPI COM, MMCSS, device hot-plug, thread lifecycle
- `src/codec/include/lumen/codec/audio_decoder.h` — `DecodedAudioFrame` struct the renderer consumes
- `tests/audio_capture_encode_test.cpp` — test wiring pattern

## Verification
1. CMake configure and build succeed with no errors
2. `audio_roundtrip_test`: full capture→encode→decode→render pipeline runs for 5 seconds
3. Stats verify: `frames_played > 0`, `underrun_count` low, `buffer_level_ms` in 20-60ms range
4. Manual: run for 30+ seconds, verify no crashes on device hot-plug, clean shutdown
