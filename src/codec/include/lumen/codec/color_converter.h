#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

namespace lumen {

/// Abstract interface for GPU color space conversion.
///
/// Converts captured frames (typically BGRA) to encoder input format
/// (typically NV12) without leaving the GPU.
class ColorConverter {
public:
    virtual ~ColorConverter() = default;

    /// Initialize the converter for the given frame dimensions.
    /// `device` is the shared platform GPU device.
    virtual Result<void> Initialize(const VideoFrameDesc& input_desc,
                                    PixelFormat output_format,
                                    void* device) = 0;

    /// Convert `input_texture` and write the result to `output_texture`.
    /// Both textures must reside on the device passed to Initialize().
    virtual Result<void> Convert(void* input_texture,
                                 void* output_texture) = 0;

    /// Get the output format this converter produces.
    virtual PixelFormat GetOutputFormat() const = 0;
};

}  // namespace lumen
