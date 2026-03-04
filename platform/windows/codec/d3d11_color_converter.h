#pragma once

#include "lumen/codec/color_converter.h"
#include "lumen/common/com_ptr.h"

#include <d3d11.h>

namespace lumen {

class D3D11DeviceContext;

/// GPU color space conversion using the D3D11 Video Processor.
///
/// Converts BGRA → NV12 using the fixed-function video processing hardware
/// present on all D3D11 GPUs. Typical latency: <1ms.
///
/// This is the universal fallback path. NVIDIA users may skip this step
/// since NVENC can accept BGRA input directly.
class D3D11ColorConverter : public ColorConverter {
public:
    explicit D3D11ColorConverter(D3D11DeviceContext& device_ctx);
    ~D3D11ColorConverter() override;

    Result<void> Initialize(const VideoFrameDesc& input_desc,
                            PixelFormat output_format,
                            void* device) override;

    Result<void> Convert(void* input_texture,
                         void* output_texture) override;

    PixelFormat GetOutputFormat() const override { return output_format_; }

private:
    D3D11DeviceContext& device_ctx_;
    PixelFormat output_format_ = PixelFormat::kNV12;

    ComPtr<ID3D11VideoProcessorEnumerator> vp_enum_;
    ComPtr<ID3D11VideoProcessor> video_processor_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}  // namespace lumen
