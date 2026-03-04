#pragma once

#include "lumen/capture/screen_capture.h"
#include "lumen/common/com_ptr.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <thread>

namespace lumen {

class D3D11DeviceContext;

/// Screen capture implementation using the DXGI Desktop Duplication API.
///
/// - Full monitor capture only (no individual window support)
/// - Polling model: call AcquireFrame() in a loop
/// - Output: ID3D11Texture2D (BGRA) on GPU, copied to an owned staging texture
/// - Handles DXGI_ERROR_ACCESS_LOST by recreating the duplication session
class DxgiDesktopDuplication : public ScreenCapture {
public:
    /// Construct with a shared device context. The device context must outlive
    /// this object and must have been initialized on the same adapter as the
    /// target display output.
    explicit DxgiDesktopDuplication(D3D11DeviceContext& device_ctx);
    ~DxgiDesktopDuplication() override;

    Result<void> Initialize(uint32_t output_index) override;
    Result<void> Start(FrameCallback callback) override;
    Result<void> Stop() override;
    Result<CapturedFrame> AcquireFrame(uint32_t timeout_ms) override;
    void ReleaseFrame() override;
    VideoFrameDesc GetFrameDesc() const override;

private:
    Result<void> CreateDuplication();
    void CaptureLoop();

    D3D11DeviceContext& device_ctx_;
    uint32_t output_index_ = 0;

    ComPtr<IDXGIOutputDuplication> duplication_;
    ComPtr<ID3D11Texture2D> staging_texture_;  // Owned texture for CopyResource

    DXGI_OUTDUPL_DESC dupl_desc_ = {};
    uint64_t frame_index_ = 0;
    bool frame_acquired_ = false;

    // Callback-driven capture loop
    FrameCallback callback_;
    std::thread capture_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace lumen
