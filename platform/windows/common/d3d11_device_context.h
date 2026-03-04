#pragma once

#include "lumen/common/com_ptr.h"
#include "lumen/common/error.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>

#include <cstdint>
#include <mutex>

namespace lumen {

/// Manages a shared ID3D11Device and associated resources that must be used
/// across capture, color conversion, and encoding for zero-copy operation.
///
/// Owns:
///   - ID3D11Device / ID3D11DeviceContext (created on the display adapter)
///   - ID3D11VideoDevice / ID3D11VideoContext (for Video Processor CSC)
///   - IMFDXGIDeviceManager (for Media Foundation encoder integration)
///
/// Thread safety: The ID3D11DeviceContext is NOT thread-safe. Callers must
/// use Lock()/Unlock() or the ScopedLock helper when issuing GPU commands.
class D3D11DeviceContext {
public:
    D3D11DeviceContext() = default;
    ~D3D11DeviceContext();

    D3D11DeviceContext(const D3D11DeviceContext&) = delete;
    D3D11DeviceContext& operator=(const D3D11DeviceContext&) = delete;

    /// Create the D3D11 device on the adapter driving the given output index.
    /// Pass output_index = 0 for the primary display.
    Result<void> Initialize(uint32_t output_index = 0);

    /// Create a staging texture suitable for CopyResource from captured frames.
    /// The texture is created with BIND_RENDER_TARGET | BIND_SHADER_RESOURCE
    /// so it can be used with encoders and video processors.
    Result<ComPtr<ID3D11Texture2D>> CreateStagingTexture(
        uint32_t width, uint32_t height, DXGI_FORMAT format);

    /// Create an NV12 texture for color conversion output.
    Result<ComPtr<ID3D11Texture2D>> CreateNV12Texture(
        uint32_t width, uint32_t height);

    ID3D11Device* Device() const { return device_.Get(); }
    ID3D11DeviceContext* Context() const { return context_.Get(); }
    ID3D11VideoDevice* VideoDevice() const { return video_device_.Get(); }
    ID3D11VideoContext* VideoContext() const { return video_context_.Get(); }
    IMFDXGIDeviceManager* DXGIDeviceManager() const { return dxgi_device_manager_.Get(); }
    IDXGIAdapter1* Adapter() const { return adapter_.Get(); }
    IDXGIOutput1* Output() const { return output_.Get(); }
    uint32_t DXGIManagerResetToken() const { return dxgi_manager_reset_token_; }

    /// RAII lock for the device context (required for multi-threaded GPU access).
    class ScopedLock {
    public:
        explicit ScopedLock(D3D11DeviceContext& ctx) : lock_(ctx.mutex_) {}
    private:
        std::unique_lock<std::mutex> lock_;
    };

private:
    Result<void> CreateDevice(uint32_t output_index);
    Result<void> CreateVideoDevice();
    Result<void> CreateDXGIDeviceManager();

    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<IMFDXGIDeviceManager> dxgi_device_manager_;
    UINT dxgi_manager_reset_token_ = 0;

    std::mutex mutex_;
};

}  // namespace lumen
