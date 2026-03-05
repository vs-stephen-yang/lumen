# Video Adaptation Research & Implementation Plan

## Context

The MfVideoEncoder is working with async MFT support (H.264, 4.5ms avg encode at 1080p60). This document covers the research and implementation plan for runtime video adaptation: dynamic bitrate/resolution/FPS changes, frame rate matching, HEVC support, and keyframe burst mitigation.

---

## 1. Dynamic Bitrate, Resolution, and FPS Changes

### 1a. Dynamic Bitrate with Rate Control Modes

The MF hardware encoder exposes these `ICodecAPI` properties for rate control, settable at runtime:

| Property GUID | Type | Purpose |
|---|---|---|
| `CODECAPI_AVEncCommonRateControlMode` | `VT_UI4` | CBR=0, PeakConstrainedVBR=1, UnconstrainedVBR=2, Quality=3 |
| `CODECAPI_AVEncCommonMeanBitRate` | `VT_UI4` | Average bitrate in bps |
| `CODECAPI_AVEncCommonMaxBitRate` | `VT_UI4` | Peak bitrate for VBR |
| `CODECAPI_AVEncCommonQuality` | `VT_UI4` | Quality level 0-100 for CQP/quality mode |
| `CODECAPI_AVEncVideoEncodeQP` | `VT_UI8` | QP value for CQP mode (NVENC, QSV) |

**Can rate control mode be changed at runtime?** No — Intel QSV MFT returns `MF_E_INVALIDREQUEST` and NVIDIA NVENC MFT also rejects mode changes after streaming starts. Bitrate/QP values are hot-changeable; mode requires MFT reinit.

**Current gap:** `RateControlMode` exists in `VideoEncoderConfig` but the encoder ignores it. Only `CODECAPI_AVEncCommonMeanBitRate` is set. Need to wire up rate control mode during `ConfigureEncoder`.

### 1b. Resolution Change

MFT does **NOT** support dynamic resolution change. There is no input-side `MF_E_TRANSFORM_STREAM_CHANGE` path for encode MFTs. Attempting to change the input media type while streaming returns `E_INVALIDARG`.

**Strategy: Controlled Teardown + Reinit**

```
1. Flush() — drain in-flight frames, wait for METransformDrainComplete
2. Stop async event loop (set drain_complete_ = true, don't re-arm BeginGetEvent)
3. Send MFT_MESSAGE_NOTIFY_END_OF_STREAM
4. Release encoder_, event_gen_, codec_api_
5. Reset async state (input_needed_count_ = 0, drain_complete_ = false)
6. FindHardwareEncoder + ConfigureEncoder with new config
7. Preserve timestamp_100ns_ continuity (do NOT reset — prevents DTS/PTS discontinuity)
8. Force keyframe on next Encode()
```

**Edge cases:**
- NV12 texture and color converter also need recreation if resolution changes (handled at pipeline layer above encoder)
- Codec change (H264→HEVC) also requires reinit since it needs a different MFT activation
- After `Flush()` waits for `METransformDrainComplete`, all pending output callbacks have fired before we release the encoder

### 1c. FPS Change

The MFT uses `MF_MT_FRAME_RATE` from the media type primarily for bitrate calculation in CBR mode (bits per frame = bitrate / fps). It does NOT enforce frame rate by rejecting frames.

**Strategy — two levels:**

1. **Timestamp-only (no reinit):** Change `frame_duration_100ns_` and let `CreateInputSample` use the new duration. The MFT bitrate controller adjusts QP within ~1 second. Sufficient for most cases.

2. **Full reinit fallback:** If the MFT rejects in-place media type update, fall back to `Reconfigure()`.

---

## 2. Frame Rate Matching (Capture FPS vs Target FPS)

### Problem

DXGI Desktop Duplication is event-driven by desktop changes:
- Screen static → no frames arrive
- Motion → frames may burst at monitor refresh rate
- Capture rate rarely matches the target encode rate exactly

The encoder's CBR controller assumes steady `fps` frames per second. Feeding it irregular timestamps causes quality oscillation.

### Solution: FramePacer

A platform-agnostic component that sits between capture and encoder:

```
FramePacer::ShouldEncode(capture_time_us) → kEncode | kDrop | kDuplicate
FramePacer::Tick(now_us) → kDuplicate (for idle screen duplication)
FramePacer::GetSampleTimestamp100ns() → synthetic MFT timestamp
```

**Algorithm:**

```
target_interval_us = 1,000,000 / target_fps

ShouldEncode(capture_time_us):
    elapsed = capture_time_us - last_encoded_time_us
    if elapsed < target_interval_us * 0.75:
        return kDrop   // Too soon — already have a frame for this slot
    last_encoded_time_us = capture_time_us
    synthetic_timestamp_ += frame_duration_
    return kEncode

Tick(now_us):
    if allow_duplication && (now_us - last_encoded_time_us) >= target_interval_us * 1.5:
        if dup_count_ < max_dup_frames:
            dup_count_++
            synthetic_timestamp_ += frame_duration_
            return kDuplicate
    return kDrop
```

**Key insight:** Always use synthetic timestamps advancing at the target frame rate, never wall-clock timestamps directly. MFT CBR bitrate control is a token-bucket tied to sample timestamps:
- Burst timestamps → MFT sees high rate → quality drops dramatically
- Gap timestamps → MFT sees gap → may produce large IDR on resume

Synthetic timestamps keep CBR stable regardless of capture jitter.

**Frame duplication for still screens:** When the screen is static and DXGI DD returns timeouts, call `Tick()`. If it returns `kDuplicate`, re-submit the last NV12 texture to the encoder. The `staging_texture_` from DXGI DD persists between frames and can be reused. With `CODECAPI_AVLowLatencyMode`, duplicated frames produce tiny P-frames (all-skip macroblocks).

---

## 3. H264 and HEVC Support

### Current State

- `CodecToSubtype` maps `kH264` → `MFVideoFormat_H264`, `kHEVC` → `MFVideoFormat_HEVC`
- `SetOutputMediaType` only sets profile/level for H264 (High, Level 4.1)
- AV1 silently falls through to H264

### HEVC Profile/Level Values

```cpp
// Set via MF_MT_MPEG2_PROFILE / MF_MT_MPEG2_LEVEL
eAVEncH265VProfile_Main_420_8  = 1   // Main profile (8-bit 4:2:0) — most compatible
eAVEncH265VProfile_Main_420_10 = 2   // Main 10 (10-bit, for HDR)
eAVEncH265VLevel4    = 120  // max 2160x2160@30 or 1920x1080@120
eAVEncH265VLevel4_1  = 123  // max 2160x2160@60 or 1920x1080@240
```

For 1080p60 encoding: Main profile + Level 4.0.

### HEVC-Specific ICodecAPI Properties

| Property | Type | Notes |
|---|---|---|
| `CODECAPI_AVEncVideoContentType` | `VT_UI4` | 0=Unknown, 1=FixedCamera, 2=ScreenContent |

Setting `CODECAPI_AVEncVideoContentType = 2` enables screen content encoding mode on Intel QSV — palette mode and intra block copy dramatically improve compression of text/UI content. Critical for screen sharing.

### HEVC Availability

Not universally available — Windows 10 S and some SKUs require the paid HEVC Video Extension from Microsoft Store. Detection: `MFTEnumEx` returns 0 activations if no hardware HEVC encoder is present.

### AV1 Fix

`MFVideoFormat_AV1` is available on Windows 11 22H2+ with supported hardware. GUID: `{30315641-0000-0010-8000-00AA00389B71}`. Map properly or return explicit error.

---

## 4. Keyframe Splitting / Large IDR Frame Mitigation

### Problem

An IDR keyframe at 1080p can be 5-20x larger than a P-frame (1-3MB vs 133KB at 8Mbps/60fps). This burst causes 100-400ms of jitter on typical networks — unacceptable for real-time streaming.

### Approach A: Slice-Based Splitting

Split each frame into multiple NAL units sized for network packets.

| Property | Type | Values |
|---|---|---|
| `CODECAPI_AVEncSliceControlMode` | `VT_UI4` | 0=Off, 1=MBs per slice, 2=Bits per slice |
| `CODECAPI_AVEncSliceControlSize` | `VT_UI4` | Max bits per slice (when mode=2) |

**Hardware support:**
- Intel QSV MFT: Full support
- NVIDIA NVENC MFT: Supported (NVENC SDK >=10), some MFT wrappers may silently ignore
- AMD VCE MFT: Limited, driver-dependent

**Output handling:** When slicing is enabled, a single `IMFSample` may contain multiple `IMFMediaBuffer` entries, one per slice. Current code uses `ConvertToContiguousBuffer` which merges them. To expose slice boundaries to the network layer, iterate `GetBufferByIndex` instead.

### Approach B: Gradual Intra Refresh (Recommended for Screen Sharing)

Instead of periodic IDR frames, use rolling intra refresh: encode a different band of macroblocks as intra-coded on every frame, cycling through the entire frame over N frames.

| Property | Type | Notes |
|---|---|---|
| `CODECAPI_AVEncVideoIntraRefreshMode` | `VT_UI4` | 0=Off, 1=Rolling, 2=Random |
| `CODECAPI_AVEncVideoIntraRefreshPeriod` | `VT_UI4` | Frames per refresh cycle (e.g., 60 = 1 sec at 60fps) |

**Advantages:** Eliminates keyframe bursts entirely — every frame is roughly the same size. Every frame is independently decodable within one refresh cycle.

**Hardware support:**
- Intel QSV MFT: Full support, well-documented
- NVIDIA NVENC MFT: Supported via `intraRefreshPeriod`
- AMD VCE: Limited

**Trade-off:** `RequestKeyframe()` still works for seek/join — the MFT sends a full IDR regardless of intra refresh state.

### Approach C: GOP Size Control

Set keyframe interval via `CODECAPI_AVEncMPVGOPSize`. Longer GOP = fewer bursts but worse join latency.

### Recommended Strategy

For screen sharing: **intra refresh as default** (eliminates bursts) + **GOP size 2-4 seconds** (for join latency) + **RequestKeyframe()** for explicit seek/join + **slice splitting as optional** for hard MTU constraints.

---

## 5. Hardware Support Matrix

| Feature | Intel QSV | NVIDIA NVENC | AMD VCE |
|---|---|---|---|
| Dynamic bitrate (CBR) | Yes | Yes | Yes |
| Dynamic bitrate (VBR) | Yes | Yes | Yes |
| Rate control mode change at runtime | No (reinit) | No (reinit) | No (reinit) |
| HEVC Main profile | Yes | Yes | Yes |
| HEVC Main 10 (HDR) | Yes | Yes | Varies |
| Screen content coding | Yes | No | No |
| Intra refresh | Yes | Yes | Limited |
| Slice control (bits per slice) | Yes | Varies | Limited |
| GOP size control | Yes | Yes | Yes |
| Low latency mode | Yes | Yes | Yes |

---

## Implementation Plan

### Step 1: Extend Config and Interface
- Add fields to `VideoEncoderConfig`: `peak_bitrate_bps`, `gop_size_frames`, `intra_refresh`, `intra_refresh_period_frames`, `max_slice_size_bytes`
- Add to `VideoEncoder`: `SetFrameRate()`, `Reconfigure()`
- Update mock encoder

### Step 2: HEVC Profile/Level
- Add HEVC branch in `SetOutputMediaType`
- Fix AV1 in `CodecToSubtype`

### Step 3: Rate Control Wiring
- Cache `ICodecAPI` as member
- Wire `config.rate_control` → `CODECAPI_AVEncCommonRateControlMode`
- Set `CODECAPI_AVEncMPVGOPSize`, `CODECAPI_AVEncMPVDefaultBPictureCount = 0`
- Respect `config.low_latency` flag

### Step 4: Intra Refresh + Slice Control
- Set properties in `ConfigureEncoder` gated on config fields
- Properties may be silently ignored — check HRESULT, log, don't fail

### Step 5: SetFrameRate
- Update `frame_duration_100ns_` + attempt in-place media type update
- Fall back to `Reconfigure()` if rejected

### Step 6: Reconfigure
- Drain → teardown → reinit sequence
- Preserve timestamp continuity

### Step 7: FramePacer
- New platform-agnostic class
- Encode/drop/duplicate decisions
- Synthetic timestamp management

### Step 8: Test Program
- `tests/video_adaptation_test.cpp` exercising all features

---

## Key References
- [ICodecAPI Interface — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nn-strmif-icodecapi)
- [Codec API Properties — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/directshow/codec-api-properties)
- [H.264 Video Encoder — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder)
- [Chromium MF Video Encode Accelerator](https://source.chromium.org/chromium/chromium/src/+/main:media/gpu/windows/media_foundation_video_encode_accelerator_win.cc)
- [OBS Studio hardware encoders](https://deepwiki.com/obsproject/obs-studio/4.4.2-hardware-video-encoders)
