# Codebase review follow-ups (2026-04-28)

Tracked items from the full-codebase review on 2026-04-28. Closed items are marked
with a date; open items have a suggested priority.

## Closed in this pass

- [x] **Convert raw COM pointers in WASAPI classes to `ComPtr`** (2026-04-28)
  - `platform/windows/renderer/wasapi_audio_renderer.{h,cpp}`
  - `platform/windows/capture/wasapi_loopback_capture.{h,cpp}`
  - `IAudioClient`, `IAudioRenderClient`/`IAudioCaptureClient`, `IMMDevice`,
    `IMMDeviceEnumerator` are now `ComPtr<>`. Manual `Release()` removed from
    destructors and `ReleaseDevice()`.
  - `IMMNotificationClient` (`DeviceNotificationClient`) deliberately left
    self-managed: callback unregister-then-`Release()` ordering is required.

- [x] **Wire `tests/e2e_pipeline_test.cpp` into the test build** (2026-04-28)
  - `tests/CMakeLists.txt` already references it (uncommitted in working tree at
    review time). The executable builds clean. Still needs `git add` to track.

## Open — code

### High

- [ ] **Reserve a version field in `MediaPacketHeader` before the wire format
      is locked in by a deployed client.**
  - File: `src/transport/include/lumen/transport/transport_types.h:82-107`,
    `src/transport/src/media_packet_header.cpp`
  - Design doc envisioned a `version:1` byte (`docs/transport-research-and-design.md:347-350`)
    that the implementation dropped. Stealing 4 bits from `flags` would let us
    add field/codec tagging later without a flag day.
  - Risk: every emitted packet today is V0; a flag day means every shipped
    receiver must be redeployed at the same time.

- [ ] **Audit `MfVideoEncoder::current_metadata_` and `MfVideoDecoder::async_decoded_frame_`
      for cross-thread synchronization.**
  - Files: `platform/windows/codec/mf_video_encoder.{h,cpp}`,
    `platform/windows/codec/mf_video_decoder.{h,cpp}`
  - The encoder's submit-thread writes `current_metadata_` and the MF event
    callback (`Invoke`) reads it on a workqueue thread. Same pattern in the
    decoder. There is an `async_mutex_` but the read/write paths around the
    metadata fields need to be confirmed to actually take it.

### Medium

- [ ] **Decoder GPU-internal copy via `staging_nv12_`.**
  - File: `platform/windows/codec/mf_video_decoder.cpp` (~lines 423-426, 549-556)
  - Decoder texture-array slices lack `BIND_SHADER_RESOURCE`, forcing a
    `CopySubresourceRegion` to a standalone NV12 texture. Stays on GPU but adds
    a copy. Investigate whether downstream samplers can bind the array slice
    directly with the right view.

- [ ] **Replace `delete this` lifetime in `DeviceNotificationClient`** (both
      WASAPI files).
  - Files: `platform/windows/renderer/wasapi_audio_renderer.cpp:25-28`,
    `platform/windows/capture/wasapi_loopback_capture.cpp:25-29`
  - Currently safe only because `UnregisterEndpointNotificationCallback` is
    called before our final `Release()`. A `ComPtr`-managed standalone object
    or a registry that owns notification clients would be less brittle.

## Open — tests

### High

- [ ] **Add unit tests for the interfaces that only have integration coverage.**
  - `video_decoder`, `audio_decoder`, `color_converter`, `transport_client`,
    `transport_server`. Today these are exercised only by real Windows backends,
    so error paths (device-lost, channel-full, timeout) aren't covered.

- [ ] **Add error-injection mocks.**
  - Missing entirely: `MockAudioDecoder`, `MockVideoDecoder`,
    `MockColorConverter`, `MockFramePacer`.
  - Existing mocks (`MockVideoEncoder`, `MockAudioEncoder`,
    `MockTransportChannel`) emit hard-coded packets and don't expose error
    injection. Extend so callers can simulate `kEncoderError`, `kDeviceLost`,
    `kTransportChannelFull`, etc.

### Medium

- [ ] **Replace sleep-based test synchronization with CV/event signaling.**
  - `tests/transport_quic_test.cpp:378,418,473,535,580` — 200 ms hard-coded waits
  - `tests/e2e_pipeline_test.cpp:159,330,627` — sleeps for pipeline drain
  - Use the existing `PacketQueue::cv` pattern (`tests/e2e_pipeline_test.cpp:65`)
    or a counter-with-CV helper.

- [ ] **Tests must not pass on missing prerequisites.**
  - `tests/encoder_test.cpp:77-79` proceeds with an uninitialized GPU input
    texture if NV12 staging cannot be created
  - `tests/audio_capture_encode_test.cpp:157-161` returns success when zero
    audio frames are produced (system silent)
  - `tests/e2e_pipeline_test.cpp:418` continues if audio capture init fails
  - Either skip cleanly with a non-zero "skipped" status or fail.

- [ ] **Add a headless skip path to `e2e_pipeline_test.cpp`.**
  - It calls `D3D11SwapChainRenderer::Initialize()` which needs a display
    output, so it cannot run on RDP/CI today. Detect and skip rather than fail.

### Low

- [ ] **Standardize the test framework / output format.**
  - The repo currently mixes a custom `TEST()` macro (transport unit tests)
    with `int main()` + printf in the integration tests. There are no
    structured assertions and no JUnit/TAP output, so CI cannot distinguish
    pass/fail/skip programmatically.

- [ ] **Latency assertions in `e2e_pipeline_test.cpp`.**
  - The histogram printout (P50/P95/P99) at lines 234-250 is good
    instrumentation. Add thresholds so a regression actually fails the test.

## Open — docs

### Medium

- [ ] **Annotate `docs/transport-research-and-design.md` with what was
      simplified vs. the implementation.**
  - `TransportConfig`: doc has `alpn`, `idle_timeout_ms`, `enable_migration`,
    `initial_rtt_ms`; impl (`transport_types.h:52-64`) has `address`, `port`,
    `send_buffer_size`, `recv_buffer_size`, `connect_timeout_ms`.
  - `TransportStats`: doc has ~19 fields incl. `min_rtt_us`, `bytes_in_flight`,
    `packets_lost`; impl (`transport_types.h:71-79`) has 7.
  - `MediaPacketHeader`: doc includes `version:1` and `payload_type:1`; impl
    (`media_packet_header.cpp:48-62`) does not — see version-field item above.

### Low

- [ ] **Mark phases 3-7 of the transport roadmap as "design only".**
  - `docs/transport-research-and-design.md:684-850`. Phase 1 (TCP+QUIC on
    Windows) and partial Phase 2 are built; jitter buffer (`src/jitter_buffer/`
    is empty), bitrate controller, ICE/STUN/TURN, cross-platform QUIC, and
    WebTransport exist only on paper.
