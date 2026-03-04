# CLAUDE.md

Lumen is a cross-platform streaming pipeline for screen sharing. It handles the full media path: screen capture, system audio capture, video/audio encoding, transport, jitter buffer, decoding, and rendering.

## Key Design Principles
- Zero-copy capture → encoding pipeline
- Zero-copy decoding → rendering pipeline
- Hardware-accelerated encoding and decoding on all platforms

## Platform Support
- Windows
- macOS
- iOS / iPadOS
- Android

## Language & Integration
- Core pipeline in C/C++
- Integrated into a Flutter project via FFI / platform channels
- Each pipeline component (capture, encoder, decoder, transport, jitter buffer, renderer) is a module behind an abstract interface
- Modules are swappable at runtime and mockable for isolated unit testing

## Build Commands
- CMake-based build system
<!-- Add specific cmake configure/build/test commands as they are established -->

## Architecture
Pipeline stages, each behind an abstract C++ interface:
1. **Screen Capture** — platform-specific backends (DXGI, ScreenCaptureKit, MediaProjection, etc.)
2. **Audio Capture** — system audio loopback per platform
3. **Video Encoder** — hardware-accelerated (Media Foundation, VideoToolbox, MediaCodec)
4. **Audio Encoder** — Opus or platform codec
5. **Transport** — send/receive encoded frames; supports QUIC and TCP backends
6. **Jitter Buffer** — reorder and smooth playback timing
7. **Video Decoder** — hardware-accelerated, zero-copy output
8. **Audio Decoder** — matching encoder codec
9. **Renderer** — zero-copy presentation (D3D, Metal, OpenGL ES / Vulkan)

## Reference Libraries
- **WebRTC** — transport, jitter buffer, congestion control
- **GStreamer** — pipeline framework, capture, encoding/decoding backends
- **FFmpeg** — codec implementations, muxing/demuxing, hardware acceleration (vaapi, nvenc, videotoolbox, mediacodec)

Component implementations can be backed by any of these libraries (e.g., an FFmpeg-based encoder vs. a GStreamer-based encoder), swapped via the abstract interface.

## Conventions
- C++17 or later
- Abstract base classes for all pipeline components; no concrete dependencies between modules
- Platform-specific code isolated in per-platform directories
- Mock implementations provided for every interface to support isolated testing
- Prefer RAII and smart pointers; no raw `new`/`delete`
