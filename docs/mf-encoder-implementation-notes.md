# Media Foundation Hardware Encoder — Implementation Notes

Notes from implementing the MF hardware video encoder (`MfVideoEncoder`) on Windows, covering issues encountered and their solutions.

---

## 1. Missing Includes

**Symptom**: Build failures on `std::vector` and `std::sort`.

**Fix**: Added `#include <vector>` to `types.h` (used by `EncodedPacket::data`) and `#include <algorithm>` to test files using `std::sort`.

---

## 2. MFT Initialization Order Matters

**Symptom**: `SetInputType` failed with `MF_E_INVALIDMEDIATYPE` when called before the DXGI device manager was set.

**Cause**: Hardware MFTs need the D3D11 device manager to know what texture formats are available on the GPU. Without it, they reject all input types.

**Fix**: Reorder initialization to: set output type → set DXGI device manager → enumerate/set input type.

---

## 3. Input Type Enumeration

**Symptom**: Hardcoding `MFVideoFormat_NV12` as the input subtype was rejected by some MFTs.

**Fix**: Enumerate available input types with `GetInputAvailableType` in a loop, preferring NV12 but falling back to whatever the MFT offers at index 0.

---

## 4. H.264 Profile and Level Required

**Symptom**: `SetOutputType` failed even with correct format/resolution/bitrate.

**Cause**: The Intel QSV MFT requires `MF_MT_MPEG2_PROFILE` and `MF_MT_MPEG2_LEVEL` on the output media type.

**Fix**: Set `eAVEncH264VProfile_High` and `eAVEncH264VLevel4_1` on the output type.

---

## 5. Async MFT Must Be Unlocked

**Symptom**: All MFT operations failed with `0xc00d6d77` after activation.

**Cause**: Hardware MFTs (NVIDIA, Intel, AMD) are typically async. Async MFTs are locked by default and must be explicitly unlocked before use.

**Fix**: Check `MF_TRANSFORM_ASYNC` attribute, then set `MF_TRANSFORM_ASYNC_UNLOCK = TRUE`.

```cpp
UINT32 is_async = 0;
mft_attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
if (is_async) {
    mft_attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
}
```

---

## 6. Async MFTs Reject Direct ProcessInput

**Symptom**: `ProcessInput` returned `MF_E_NOTACCEPTING` (0xc00d36b5). Retry loops with `Sleep` did not help.

**Cause**: Async MFTs use an event-driven model. You cannot call `ProcessInput` until the MFT fires a `METransformNeedInput` event. Synchronous `GetEvent(0)` deadlocks on async MFTs because the events are delivered via the async callback mechanism.

**Fix**: Implement `IMFAsyncCallback` with `BeginGetEvent`/`EndGetEvent`/`Invoke`. See next section.

---

## 7. IMFAsyncCallback Event Loop Pattern

The correct pattern for async MFTs (matching Chromium's `MediaFoundationVideoEncodeAccelerator`):

1. **Start the event loop** after `NOTIFY_START_OF_STREAM`:
   ```cpp
   event_gen_->BeginGetEvent(this, nullptr);
   ```

2. **`Invoke()` callback** handles three event types:
   - `METransformNeedInput` (601) — increment `input_needed_count_`, signal condition variable
   - `METransformHaveOutput` (602) — call `ProcessOutput` to extract encoded data
   - `METransformDrainComplete` (603) — signal drain condition variable

3. **Continue the loop** at the end of each `Invoke()`:
   ```cpp
   event_gen_->BeginGetEvent(this, nullptr);
   ```

4. **`EncodeAsyncMft()`** waits on the condition variable for `input_needed_count_ > 0` before calling `ProcessInput`.

5. **IUnknown**: Must implement `QueryInterface`/`AddRef`/`Release` since the MF work queue holds a reference to the callback. Use `std::atomic<ULONG>` for the ref count but do NOT `delete this` in `Release` since the object is not COM-allocated.

---

## 8. MF_E_TRANSFORM_STREAM_CHANGE on First Output

**Symptom**: First `METransformHaveOutput` event fires, but `ProcessOutput` returns `MF_E_TRANSFORM_STREAM_CHANGE` (0xc00d6d61) instead of encoded data. No further `METransformNeedInput` events arrive.

**Cause**: After receiving the first input frame, the MFT determines the actual output format (which may differ from what was initially negotiated) and signals a stream change. This is standard MFT behavior.

**Fix**: When `ProcessOutput` returns `MF_E_TRANSFORM_STREAM_CHANGE`:
```cpp
ComPtr<IMFMediaType> new_type;
encoder_->GetOutputAvailableType(output_stream_id_, 0, &new_type);
encoder_->SetOutputType(output_stream_id_, new_type.Get(), 0);
```
After renegotiation, the MFT resumes normal operation and fires `METransformHaveOutput` again with actual encoded data.

---

## 9. DXGI Desktop Duplication Requires Physical Console

**Symptom**: `DuplicateOutput` returns `E_ACCESSDENIED` (0x80070005).

**Cause**: DXGI Desktop Duplication does not work in RDP sessions.

**Workaround**: Created a standalone encoder test (`encoder_test.cpp`) that feeds synthetic NV12 frames via a staging texture, bypassing the capture stage entirely. This allows testing the encoder pipeline in any session type.

---

## Final Performance (1920x1080, H.264 High, 8 Mbps, Low-Latency)

| Metric | Value |
|--------|-------|
| Avg encode latency | 4.53 ms |
| P50 | 4.46 ms |
| P95 | 7.75 ms |
| P99 | 7.87 ms |
| Effective FPS | 220.6 |
| Output | 300 packets, 3.81 MB, 3 keyframes |

Bitstream verified: valid H.264 NAL structure (SPS/PPS/IDR/non-IDR slices with correct start codes).
