# Research: Zero-Copy Capture & Encoding on Windows

## Context
Lumen needs a zero-copy screen capture → encoding pipeline on Windows. This document covers the available APIs, implementation strategy, and known challenges.

---

## 1. Capture APIs

### DXGI Desktop Duplication (Win 8+)
- **Interfaces**: `IDXGIOutput1::DuplicateOutput`, `IDXGIOutput5::DuplicateOutput1` (format selection), `IDXGIOutputDuplication`
- **Frame acquisition**: `AcquireNextFrame(timeout, &frameInfo, &resource)` → `QueryInterface` to `ID3D11Texture2D`
- **Output format**: `DXGI_FORMAT_B8G8R8A8_UNORM` (BGRA). With `DuplicateOutput1` you can request native HDR formats (`R16G16B16A16_FLOAT`)
- **Scope**: Full monitor only (no individual window capture)
- **Threading**: Caller's thread, polling model
- **Constraints**: Max 4 concurrent sessions system-wide; `DXGI_ERROR_ACCESS_LOST` on desktop switch / mode change (must recreate session); broken in most RDP sessions

### Windows.Graphics.Capture (Win 10 1803+)
- **Classes**: `GraphicsCaptureItem`, `Direct3D11CaptureFramePool`, `GraphicsCaptureSession`
- **Frame access**: `FrameArrived` event → `TryGetNextFrame()` → `IDirect3DSurface` → unwrap to `ID3D11Texture2D`
- **Output format**: BGRA (configurable pixel format in frame pool)
- **Scope**: Individual windows OR full monitors
- **Permission**: Picker UI required by default; programmatic access via `GraphicsCaptureAccess` (Win 10 1903+); yellow border on by default (removable on Win 11)
- **Threading**: `CreateFreeThreaded` avoids DispatcherQueue requirement

### Comparison

| Aspect | DXGI DD | WGC |
|---|---|---|
| Window capture | No | Yes |
| Permission | None | Picker UI / programmatic |
| HDR | Via `DuplicateOutput1` | Via pixel format param |
| Frame delivery | Polling | Event-driven |
| Cross-GPU | Must match display adapter | Handled by DWM internally |
| UWP compatible | No | Yes |

**Both APIs deliver `ID3D11Texture2D` on GPU. The downstream encoding path is identical.**

---

## 2. Zero-Copy Pipeline Architecture

```
Capture (GPU)                  Color Convert (GPU)              Encode (GPU)
─────────────                  ───────────────────              ────────────

DXGI DD / WGC                  D3D11 Video Processor            NVENC / AMF / MFT
      │                        or NVENC internal CSC                  │
      ▼                              │                               ▼
ID3D11Texture2D (BGRA)  ──►  ID3D11Texture2D (NV12)  ──►  H.264/HEVC bitstream
    [VRAM]                       [VRAM]                        [CPU buffer]
                                                                     │
    ◄── Same ID3D11Device via IMFDXGIDeviceManager ──►               ▼
                                                              Network transport
```

**Critical requirement**: A single `ID3D11Device` must be shared across capture, conversion, and encoding. Create it on the adapter that owns the display output.

---

## 3. Color Space Conversion (BGRA → NV12)

Three GPU-only approaches:

### A. D3D11 Video Processor (universal)
```cpp
ID3D11VideoDevice → CreateVideoProcessorEnumerator → CreateVideoProcessor
ID3D11VideoContext::VideoProcessorBlt(processor, outputView_NV12, 0, 1, &stream_BGRA)
```
Fixed-function hardware unit, <1ms. Works on all vendors.

### B. NVENC internal conversion (NVIDIA only)
Register BGRA texture directly with `NV_ENC_BUFFER_FORMAT_ARGB`. NVENC converts internally — no explicit conversion step needed.

### C. Custom pixel shaders
Two-pass: Y plane (`R8` target) + UV plane (`R8G8` target). More control but more code.

**Recommendation**: Use (B) for NVIDIA, (A) for AMD/Intel/fallback.

---

## 4. Hardware Encoder Paths

### 4a. NVIDIA NVENC (direct SDK)
```
NvEncOpenEncodeSessionEx(d3d11Device)
NvEncRegisterResource(texture, NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX)
Per frame: NvEncMapInputResource → NvEncEncodePicture → NvEncUnmapInputResource
```
- Accepts BGRA directly (internal NV12 conversion)
- Most reliable zero-copy path; well-documented SDK (`NvEncoderD3D11` helper class)
- **Gotcha**: DD textures have incompatible bind flags — need one `CopyResource` to a texture you control (GPU-internal, <1ms)
- Low-latency: `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`, zero B-frames, zero lookahead

### 4b. AMD AMF
```cpp
AMFContext::InitDX11(d3d11Device)
AMFContext::CreateSurfaceFromDX11Native(texture) → AMFComponent::SubmitInput
```
- Accepts D3D11 textures natively (BGRA or NV12)
- Uses `IDXGIKeyedMutex` for shared texture synchronization
- OBS-style texture pool with observer-pattern recycling
- Low-latency: `AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY`

### 4c. Intel QSV via Media Foundation MFT
```cpp
MFCreateDXGIDeviceManager(&resetToken, &deviceManager)
deviceManager->ResetDevice(d3d11Device, resetToken)
encoderMFT->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager)

// Per frame:
MFCreateDXGISurfaceBuffer(IID_ID3D11Texture2D, nv12Texture, 0, FALSE, &buffer)
MFCreateSample → AddBuffer → ProcessInput
```
- Requires explicit NV12 conversion first (D3D11 Video Processor)
- `CODECAPI_AVLowLatencyMode = TRUE` is critical (otherwise 30ms+ buffering)
- Some driver versions have bugs with D3D11 texture input

### 4d. Media Foundation Generic (fallback)
```cpp
MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, ...)
```
Discovers whatever hardware encoder is available. Same `IMFDXGIDeviceManager` pattern as QSV.

---

## 5. FFmpeg & GStreamer Integration

### FFmpeg
- **`ddagrab`** filter (FFmpeg 6.0+): DXGI DD capture returning D3D11 hardware frames — no CPU download
- Zero-copy encode: `ffmpeg -f lavfi -i ddagrab=framerate=60 -c:v h264_nvenc -tune ll output.mp4`
- Also works with `h264_amf` (AMD) and `h264_qsv` (Intel)
- `d3d11va` hwframes context manages texture pools

### GStreamer
- **`d3d11screencapturesrc`**: supports both DD and WGC backends (`capture-api` property)
- Negotiates `video/x-raw(memory:D3D11Memory)` caps — GPU textures pass through pipeline without copies
- Pipeline: `d3d11screencapturesrc ! d3d11convert ! mfh264enc ! h264parse ! mp4mux ! filesink`
- `d3d12screencapturesrc` (GStreamer 1.24+) offers better perf via D3D11/D3D12 sharing

---

## 6. Known Challenges

| Challenge | Details | Mitigation |
|---|---|---|
| **Cross-adapter GPU** | Laptop iGPU+dGPU: capture on display adapter, encoder on different GPU | Detect display adapter, create device on it; or use shared handle (`CreateSharedHandle` + `OpenSharedResource1`) with PCIe penalty |
| **DD texture bind flags** | NVENC rejects DD textures directly (black frames) | One `CopyResource` to your own texture (<1ms GPU blit) |
| **Variable frame rate** | DD only fires on desktop change; idle = no frames | Implement frame duplication for fixed-rate encoder input |
| **HDCP/DRM content** | Protected windows render as black in both APIs | No workaround — by design |
| **Multi-monitor** | Separate `IDXGIOutputDuplication` per output, independent refresh rates | Independent capture loops per monitor |
| **HDR** | Desktop format changes to `R16G16B16A16_FLOAT` (scRGB) | Tone-map to SDR via Video Processor/shader, or encode HEVC Main 10 with HDR metadata |
| **`ACCESS_LOST`** | Desktop switch, mode change, DWM restart | Detect error, teardown session, recreate |
| **MFT quirks** | Some vendor MFTs silently accept input but produce no output with wrong texture descriptors | Test per-vendor; prefer direct NVENC/AMF SDKs |
| **Win 11 24H2 bug** | `d3d11screencapturesrc` stops on static screens | Known GStreamer issue; use DD polling mode as workaround |

---

## 7. Latency Budget

| Stage | Typical | Notes |
|---|---|---|
| DWM composition | 6–16ms | Unavoidable (VSync-locked) |
| `AcquireNextFrame` | 0–16ms | Poll aggressively with short timeout |
| `CopyResource` (same device) | <1ms | GPU-internal blit |
| Color conversion (Video Processor) | <1ms | Fixed-function |
| Hardware encode | 1–8ms | 1ms with NVENC ultra-low-latency |
| **Total capture-to-bitstream** | **~10–25ms** | At 60Hz monitor |

---

## 8. Recommended Strategy for Lumen

```
DXGI Desktop Duplication (primary) / WGC (for window capture)
    │
    ▼
CopyResource to owned texture (GPU, <1ms)
    │
    ├── NVIDIA: BGRA → NVENC (internal CSC) → H.264/HEVC
    ├── AMD:    BGRA → AMF encoder → H.264/HEVC
    ├── Intel:  BGRA → D3D11 Video Processor → NV12 → MFT → H.264/HEVC
    └── Fallback: → D3D11 Video Processor → NV12 → generic hardware MFT
```

This matches the architecture of OBS Studio, Sunshine, and Parsec.

---

## Key References
- [Desktop Duplication API — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)
- [NVIDIA Video Codec SDK / NVENC Programming Guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/)
- [AMD AMF SDK — GitHub](https://github.com/GPUOpen-LibrariesAndSDKs/AMF)
- [DXGICaptureDXColorSpaceConversionIntelEncode — GitHub](https://github.com/bavulapati/DXGICaptureDXColorSpaceConversionIntelEncode)
- [FFmpeg ddagrab docs](https://ayosec.github.io/ffmpeg-filters-docs/7.1/Sources/Video/ddagrab.html)
- [GStreamer d3d11screencapturesrc](https://gstreamer.freedesktop.org/documentation/d3d11/d3d11screencapturesrc.html)
- [OBS Studio hardware encoders](https://deepwiki.com/obsproject/obs-studio/4.4.2-hardware-video-encoders)
