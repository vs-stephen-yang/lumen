#include "dxgi_desktop_duplication.h"
#include "../common/d3d11_device_context.h"

#include <chrono>

namespace lumen {

DxgiDesktopDuplication::DxgiDesktopDuplication(D3D11DeviceContext& device_ctx)
    : device_ctx_(device_ctx) {}

DxgiDesktopDuplication::~DxgiDesktopDuplication() {
    Stop();
}

Result<void> DxgiDesktopDuplication::Initialize(uint32_t output_index) {
    output_index_ = output_index;
    return CreateDuplication();
}

Result<void> DxgiDesktopDuplication::CreateDuplication() {
    // Release any existing duplication session.
    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
    duplication_.Reset();
    staging_texture_.Reset();

    // Create the duplication session on the output.
    IDXGIOutput1* output = device_ctx_.Output();
    if (!output) {
        return Error::Make(ErrorCode::kNotInitialized,
                           "D3D11DeviceContext has no output");
    }

    HRESULT hr = output->DuplicateOutput(device_ctx_.Device(), &duplication_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "DuplicateOutput failed: HRESULT 0x" +
                           std::to_string(static_cast<unsigned long>(hr)));
    }

    duplication_->GetDesc(&dupl_desc_);

    // Create the staging texture we copy captured frames into.
    // DD textures have restricted bind flags, so we need our own texture
    // with BIND_RENDER_TARGET | BIND_SHADER_RESOURCE for downstream use.
    auto tex_result = device_ctx_.CreateStagingTexture(
        dupl_desc_.ModeDesc.Width,
        dupl_desc_.ModeDesc.Height,
        dupl_desc_.ModeDesc.Format);
    if (!tex_result) return tex_result.error();

    staging_texture_ = std::move(tex_result).value();
    frame_index_ = 0;

    return {};
}

Result<void> DxgiDesktopDuplication::Start(FrameCallback callback) {
    if (running_) {
        return Error::Make(ErrorCode::kAlreadyInitialized,
                           "Capture already running");
    }
    if (!duplication_) {
        return Error::Make(ErrorCode::kNotInitialized,
                           "Not initialized");
    }

    callback_ = std::move(callback);
    running_ = true;

    capture_thread_ = std::thread([this] { CaptureLoop(); });
    return {};
}

Result<void> DxgiDesktopDuplication::Stop() {
    running_ = false;
    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    return {};
}

Result<CapturedFrame> DxgiDesktopDuplication::AcquireFrame(uint32_t timeout_ms) {
    if (!duplication_) {
        return Error::Make(ErrorCode::kNotInitialized, "Not initialized");
    }

    // Release the previous frame if still held.
    if (frame_acquired_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }

    DXGI_OUTDUPL_FRAME_INFO frame_info = {};
    ComPtr<IDXGIResource> desktop_resource;

    HRESULT hr = duplication_->AcquireNextFrame(
        timeout_ms, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return Error::Make(ErrorCode::kTimeout, "No new frame");
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Session lost — recreate.
        auto recreate_result = CreateDuplication();
        if (!recreate_result) return recreate_result.error();
        return Error::Make(ErrorCode::kAccessLost,
                           "Session recreated after ACCESS_LOST; retry");
    }

    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceLost,
                           "AcquireNextFrame failed");
    }

    frame_acquired_ = true;

    // QI to ID3D11Texture2D.
    ComPtr<ID3D11Texture2D> desktop_texture;
    hr = desktop_resource.As(&desktop_texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
        return Error::Make(ErrorCode::kDeviceLost,
                           "QueryInterface ID3D11Texture2D failed");
    }

    // GPU-internal copy to our staging texture (fixes bind flag incompatibility).
    {
        D3D11DeviceContext::ScopedLock lock(device_ctx_);
        device_ctx_.Context()->CopyResource(
            staging_texture_.Get(), desktop_texture.Get());
    }

    // Capture timestamp.
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    CapturedFrame frame;
    frame.native_texture = staging_texture_.Get();
    frame.desc.width = dupl_desc_.ModeDesc.Width;
    frame.desc.height = dupl_desc_.ModeDesc.Height;
    frame.desc.format = PixelFormat::kBGRA;
    frame.metadata.frame_index = frame_index_++;
    frame.metadata.capture_time_us = us;

    return frame;
}

void DxgiDesktopDuplication::ReleaseFrame() {
    if (frame_acquired_ && duplication_) {
        duplication_->ReleaseFrame();
        frame_acquired_ = false;
    }
}

VideoFrameDesc DxgiDesktopDuplication::GetFrameDesc() const {
    VideoFrameDesc desc;
    desc.width = dupl_desc_.ModeDesc.Width;
    desc.height = dupl_desc_.ModeDesc.Height;
    desc.format = PixelFormat::kBGRA;
    return desc;
}

void DxgiDesktopDuplication::CaptureLoop() {
    while (running_) {
        auto result = AcquireFrame(16);  // ~60Hz polling
        if (result.ok()) {
            callback_(result.value());
        }
        // On timeout or access_lost, just continue the loop.
        // Access lost triggers session recreation inside AcquireFrame().
    }
}

}  // namespace lumen
