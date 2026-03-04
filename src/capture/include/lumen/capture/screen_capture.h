#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace lumen {

/// Opaque handle to a GPU texture owned by the capture backend.
/// Platform implementations wrap the native texture (e.g. ID3D11Texture2D).
struct CapturedFrame {
    void* native_texture = nullptr;   // Platform-specific GPU texture pointer
    VideoFrameDesc desc;
    FrameMetadata metadata;
};

/// Callback invoked when a new frame is captured.
using FrameCallback = std::function<void(const CapturedFrame&)>;

/// Abstract interface for screen capture backends.
///
/// Implementations:
///   - DxgiDesktopDuplication (Windows, full monitor)
///   - WgcScreenCapture       (Windows, window or monitor)
///   - ScreenCaptureKit       (macOS)
///   - MediaProjection        (Android)
class ScreenCapture {
public:
    virtual ~ScreenCapture() = default;

    /// Initialize the capture session for the given monitor/output index.
    virtual Result<void> Initialize(uint32_t output_index) = 0;

    /// Start capturing. Frames are delivered via the callback.
    virtual Result<void> Start(FrameCallback callback) = 0;

    /// Stop capturing and release the session.
    virtual Result<void> Stop() = 0;

    /// Poll for the next frame (for polling-based backends like DXGI DD).
    /// Returns kTimeout if no new frame is available within timeout_ms.
    virtual Result<CapturedFrame> AcquireFrame(uint32_t timeout_ms) = 0;

    /// Release a previously acquired frame back to the capture backend.
    virtual void ReleaseFrame() = 0;

    /// Query the current output dimensions and format.
    virtual VideoFrameDesc GetFrameDesc() const = 0;
};

}  // namespace lumen
