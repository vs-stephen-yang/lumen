# Windows Zero-Copy Decode & Rendering

## Overview

This document captures the research and design decisions for the decode → render
path on Windows. The goal is to receive an H.264/HEVC bitstream, hardware-decode
it to a GPU texture, and present it on screen — all without CPU copies.

## Chosen Approach: Solution A

**Media Foundation Decoder + D3D11 Video Processor + SwapChain HWND**

```
H.264 bitstream → MF Hardware Decoder → ID3D11Texture2D (NV12, texture array)
    → CopySubresourceRegion to standalone NV12 texture
    → D3D11 VideoProcessorBlt (NV12 → BGRA) → SwapChain back buffer → Present
```

### Why This Approach

1. Mirrors the existing MfVideoEncoder implementation — fastest to build
2. Reuses D3D11DeviceContext, IMFDXGIDeviceManager, D3D11ColorConverter
3. No new dependencies (Windows SDK only)
4. Lowest latency path (~3-5ms end-to-end)
5. SwapChain rendering is simplest to test

### Key Implementation Details

- **Texture bind flags**: Decoder output textures are in a texture array and
  lack `D3D11_BIND_SHADER_RESOURCE`. We use `CopySubresourceRegion` to copy
  each decoded frame to a standalone NV12 texture before color conversion.
- **Async MFT support**: Same `IMFAsyncCallback` pattern as the encoder.
- **Low-latency**: `MF_LOW_LATENCY` attribute + `CODECAPI_AVLowLatencyMode`.
- **Stream change**: `MF_E_TRANSFORM_STREAM_CHANGE` handled on first output
  (same as encoder).

## Future Evolution

- **Solution B**: Replace SwapChain renderer with Flutter GPU Texture (shared
  handle) for native Flutter widget integration.
- **Solution C**: FFmpeg d3d11va decoder for cross-platform support and
  HEVC/AV1 without Microsoft Store extensions.

## API Alternatives Considered

### FFmpeg d3d11va
- Multi-codec support (H.264/HEVC/AV1 out of the box)
- LGPL licensing concern
- Same texture bind flag issue
- Better for cross-platform abstraction

### GStreamer d3d11 Plugins
- Full pipeline framework with built-in video sink
- Heaviest dependency
- Hardest to integrate with Flutter

## NV12 → BGRA Conversion

Uses the D3D11 Video Processor (same hardware as capture path, reverse direction).
The `VideoProcessorBlt` API is format-agnostic — it handles NV12→BGRA and
BGRA→NV12 with the same call pattern. Typical latency: <1ms.

## Architecture

| Component | Interface | Implementation |
|-----------|-----------|----------------|
| Decoder | `VideoDecoder` (src/codec/) | `MfVideoDecoder` (platform/windows/codec/) |
| Color Converter | `ColorConverter` (src/codec/) | `D3D11ColorConverter` (platform/windows/codec/) |
| Renderer | `VideoRenderer` (src/renderer/) | `D3D11SwapChainRenderer` (platform/windows/renderer/) |
