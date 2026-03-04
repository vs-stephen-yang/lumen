#include "d3d11_device_context.h"

#include <dxgi1_6.h>

#include <sstream>

namespace lumen {

namespace {

std::string HResultToString(HRESULT hr) {
    std::ostringstream oss;
    oss << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

}  // namespace

D3D11DeviceContext::~D3D11DeviceContext() = default;

Result<void> D3D11DeviceContext::Initialize(uint32_t output_index) {
    auto result = CreateDevice(output_index);
    if (!result) return result;

    result = CreateVideoDevice();
    if (!result) return result;

    result = CreateDXGIDeviceManager();
    if (!result) return result;

    return {};
}

Result<void> D3D11DeviceContext::CreateDevice(uint32_t output_index) {
    // Enumerate adapters to find the one driving the requested output.
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "CreateDXGIFactory1 failed: " + HResultToString(hr));
    }

    // Walk adapters and outputs to find the target display.
    ComPtr<IDXGIAdapter1> target_adapter;
    ComPtr<IDXGIOutput> target_output;
    uint32_t current_output = 0;

    for (UINT adapter_idx = 0; ; ++adapter_idx) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_idx, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;

        for (UINT output_idx = 0; ; ++output_idx) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_idx, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) break;

            if (current_output == output_index) {
                target_adapter = adapter;
                target_output = output;
                break;
            }
            ++current_output;
        }
        if (target_adapter) break;
    }

    if (!target_adapter) {
        return Error::Make(ErrorCode::kInvalidArgument,
                           "Output index " + std::to_string(output_index) + " not found");
    }

    hr = target_adapter.As(&adapter_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "QueryInterface IDXGIAdapter1 failed");
    }

    hr = target_output.As(&output_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "QueryInterface IDXGIOutput1 failed");
    }

    // Create the D3D11 device on the target adapter.
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL actual_level;
    hr = D3D11CreateDevice(
        target_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,  // Must use UNKNOWN when specifying adapter
        nullptr,
        create_flags,
        feature_levels,
        _countof(feature_levels),
        D3D11_SDK_VERSION,
        &device_,
        &actual_level,
        &context_);

    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "D3D11CreateDevice failed: " + HResultToString(hr));
    }

    // Enable multithreaded device access.
    ComPtr<ID3D10Multithread> multithread;
    hr = device_.As(&multithread);
    if (SUCCEEDED(hr)) {
        multithread->SetMultithreadProtected(TRUE);
    }

    return {};
}

Result<void> D3D11DeviceContext::CreateVideoDevice() {
    HRESULT hr = device_.As(&video_device_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "QueryInterface ID3D11VideoDevice failed: " + HResultToString(hr));
    }

    hr = context_.As(&video_context_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "QueryInterface ID3D11VideoContext failed: " + HResultToString(hr));
    }

    return {};
}

Result<void> D3D11DeviceContext::CreateDXGIDeviceManager() {
    HRESULT hr = MFCreateDXGIDeviceManager(
        &dxgi_manager_reset_token_, &dxgi_device_manager_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "MFCreateDXGIDeviceManager failed: " + HResultToString(hr));
    }

    hr = dxgi_device_manager_->ResetDevice(device_.Get(), dxgi_manager_reset_token_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "IMFDXGIDeviceManager::ResetDevice failed: " + HResultToString(hr));
    }

    return {};
}

Result<ComPtr<ID3D11Texture2D>> D3D11DeviceContext::CreateStagingTexture(
    uint32_t width, uint32_t height, DXGI_FORMAT format) {

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "CreateTexture2D (staging) failed: " + HResultToString(hr));
    }
    return texture;
}

Result<ComPtr<ID3D11Texture2D>> D3D11DeviceContext::CreateNV12Texture(
    uint32_t width, uint32_t height) {

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDeviceCreationFailed,
                           "CreateTexture2D (NV12) failed: " + HResultToString(hr));
    }
    return texture;
}

}  // namespace lumen
