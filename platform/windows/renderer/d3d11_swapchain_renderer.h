#pragma once

#include "lumen/renderer/video_renderer.h"
#include "lumen/common/com_ptr.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace lumen {

class D3D11DeviceContext;
class D3D11ColorConverter;

/// Video renderer using a DXGI SwapChain presented to a Win32 window.
///
/// Performs NV12 → BGRA color conversion via D3D11 Video Processor, then
/// presents the result through the swap chain. Lowest-latency path.
class D3D11SwapChainRenderer : public VideoRenderer {
public:
    explicit D3D11SwapChainRenderer(D3D11DeviceContext& device_ctx,
                                    D3D11ColorConverter& color_converter);
    ~D3D11SwapChainRenderer() override;

    Result<void> Initialize(uint32_t width, uint32_t height,
                            void* device) override;

    Result<void> RenderFrame(void* native_texture,
                             PixelFormat format) override;

    Result<void> Present() override;

    bool IsWindowClosed() const override;

private:
    Result<void> CreateOutputWindow(uint32_t width, uint32_t height);
    Result<void> CreateSwapChain(uint32_t width, uint32_t height);

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg,
                                        WPARAM wparam, LPARAM lparam);

    D3D11DeviceContext& device_ctx_;
    D3D11ColorConverter& color_converter_;

    HWND hwnd_ = nullptr;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11Texture2D> back_buffer_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool window_closed_ = false;
};

}  // namespace lumen
