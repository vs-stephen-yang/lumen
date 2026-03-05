#include "d3d11_swapchain_renderer.h"
#include "../common/d3d11_device_context.h"
#include "../codec/d3d11_color_converter.h"

#include <sstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace lumen {

namespace {

std::string HrToStr(HRESULT hr) {
    std::ostringstream oss;
    oss << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

}  // namespace

D3D11SwapChainRenderer::D3D11SwapChainRenderer(
    D3D11DeviceContext& device_ctx,
    D3D11ColorConverter& color_converter)
    : device_ctx_(device_ctx),
      color_converter_(color_converter) {}

D3D11SwapChainRenderer::~D3D11SwapChainRenderer() {
    back_buffer_.Reset();
    swap_chain_.Reset();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

Result<void> D3D11SwapChainRenderer::Initialize(uint32_t width, uint32_t height,
                                                 void* /*device*/) {
    width_ = width;
    height_ = height;

    auto result = CreateOutputWindow(width, height);
    if (!result) return result;

    result = CreateSwapChain(width, height);
    if (!result) return result;

    return {};
}

Result<void> D3D11SwapChainRenderer::CreateOutputWindow(uint32_t width, uint32_t height) {
    const wchar_t* class_name = L"LumenDecoderWindow";

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

    // Register class (ignore if already registered).
    RegisterClassExW(&wc);

    // Calculate window size to fit the client area.
    RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(
        0,
        class_name,
        L"Lumen Decoder Output",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr, nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!hwnd_) {
        return Error::Make(ErrorCode::kRendererError,
                           "CreateWindowEx failed");
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    return {};
}

Result<void> D3D11SwapChainRenderer::CreateSwapChain(uint32_t width, uint32_t height) {
    // Get the DXGI factory from the device's adapter.
    ComPtr<IDXGIDevice1> dxgi_device;
    HRESULT hr = device_ctx_.Device()->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kRendererError,
                           "QueryInterface IDXGIDevice1 failed: " + HrToStr(hr));
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kRendererError,
                           "GetAdapter failed: " + HrToStr(hr));
    }

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kRendererError,
                           "GetParent IDXGIFactory2 failed: " + HrToStr(hr));
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = width;
    sc_desc.Height = height;
    sc_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc_desc.Flags = 0;

    hr = factory->CreateSwapChainForHwnd(
        device_ctx_.Device(), hwnd_, &sc_desc, nullptr, nullptr, &swap_chain_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kRendererError,
                           "CreateSwapChainForHwnd failed: " + HrToStr(hr));
    }

    // Get the back buffer texture.
    hr = swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer_));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kRendererError,
                           "GetBuffer(0) failed: " + HrToStr(hr));
    }

    return {};
}

// ---------------------------------------------------------------------------
// Render and Present
// ---------------------------------------------------------------------------

Result<void> D3D11SwapChainRenderer::RenderFrame(void* native_texture,
                                                  PixelFormat format) {
    if (window_closed_) {
        return Error::Make(ErrorCode::kRendererError, "Window is closed");
    }

    if (format == PixelFormat::kNV12) {
        // Use color converter: NV12 → BGRA directly to the back buffer.
        return color_converter_.Convert(native_texture, back_buffer_.Get());
    }

    // For BGRA input, just copy directly.
    auto* texture = static_cast<ID3D11Texture2D*>(native_texture);
    {
        D3D11DeviceContext::ScopedLock lock(device_ctx_);
        device_ctx_.Context()->CopyResource(back_buffer_.Get(), texture);
    }

    return {};
}

Result<void> D3D11SwapChainRenderer::Present() {
    if (window_closed_) {
        return Error::Make(ErrorCode::kRendererError, "Window is closed");
    }

    // Pump window messages.
    MSG msg;
    while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    HRESULT hr = swap_chain_->Present(1, 0);  // VSync
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kRendererError,
                           "Present failed: " + HrToStr(hr));
    }

    return {};
}

bool D3D11SwapChainRenderer::IsWindowClosed() const {
    return window_closed_;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK D3D11SwapChainRenderer::WindowProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {

    D3D11SwapChainRenderer* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
        self = static_cast<D3D11SwapChainRenderer*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<D3D11SwapChainRenderer*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_CLOSE:
            if (self) self->window_closed_ = true;
            return 0;

        case WM_DESTROY:
            if (self) self->window_closed_ = true;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace lumen
