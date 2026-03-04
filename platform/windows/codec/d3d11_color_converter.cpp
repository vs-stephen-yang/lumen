#include "d3d11_color_converter.h"
#include "../common/d3d11_device_context.h"

namespace lumen {

D3D11ColorConverter::D3D11ColorConverter(D3D11DeviceContext& device_ctx)
    : device_ctx_(device_ctx) {}

D3D11ColorConverter::~D3D11ColorConverter() = default;

Result<void> D3D11ColorConverter::Initialize(
    const VideoFrameDesc& input_desc,
    PixelFormat output_format,
    void* /*device*/) {

    if (output_format != PixelFormat::kNV12) {
        return Error::Make(ErrorCode::kUnsupported,
                           "Only NV12 output is supported");
    }

    width_ = input_desc.width;
    height_ = input_desc.height;
    output_format_ = output_format;

    // Describe the video processor capabilities we need.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content_desc = {};
    content_desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content_desc.InputWidth = width_;
    content_desc.InputHeight = height_;
    content_desc.OutputWidth = width_;
    content_desc.OutputHeight = height_;
    content_desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = device_ctx_.VideoDevice()->CreateVideoProcessorEnumerator(
        &content_desc, &vp_enum_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kColorConversionError,
                           "CreateVideoProcessorEnumerator failed");
    }

    hr = device_ctx_.VideoDevice()->CreateVideoProcessor(
        vp_enum_.Get(), 0, &video_processor_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kColorConversionError,
                           "CreateVideoProcessor failed");
    }

    // Disable auto-processing features that add latency.
    device_ctx_.VideoContext()->VideoProcessorSetStreamAutoProcessingMode(
        video_processor_.Get(), 0, FALSE);

    return {};
}

Result<void> D3D11ColorConverter::Convert(
    void* input_texture, void* output_texture) {

    auto* bgra_texture = static_cast<ID3D11Texture2D*>(input_texture);
    auto* nv12_texture = static_cast<ID3D11Texture2D*>(output_texture);

    // Create input view (BGRA source).
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_view_desc = {};
    input_view_desc.FourCC = 0;
    input_view_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_view_desc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorInputView> input_view;
    HRESULT hr = device_ctx_.VideoDevice()->CreateVideoProcessorInputView(
        bgra_texture, vp_enum_.Get(), &input_view_desc, &input_view);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kColorConversionError,
                           "CreateVideoProcessorInputView failed");
    }

    // Create output view (NV12 target).
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_view_desc = {};
    output_view_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    output_view_desc.Texture2D.MipSlice = 0;

    ComPtr<ID3D11VideoProcessorOutputView> output_view;
    hr = device_ctx_.VideoDevice()->CreateVideoProcessorOutputView(
        nv12_texture, vp_enum_.Get(), &output_view_desc, &output_view);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kColorConversionError,
                           "CreateVideoProcessorOutputView failed");
    }

    // Execute the blit (BGRA → NV12).
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = input_view.Get();

    {
        D3D11DeviceContext::ScopedLock lock(device_ctx_);
        hr = device_ctx_.VideoContext()->VideoProcessorBlt(
            video_processor_.Get(), output_view.Get(), 0, 1, &stream);
    }

    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kColorConversionError,
                           "VideoProcessorBlt failed");
    }

    return {};
}

}  // namespace lumen
