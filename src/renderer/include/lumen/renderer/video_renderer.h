#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <cstdint>

namespace lumen {

/// Abstract interface for video frame presentation.
///
/// Implementations:
///   - D3D11SwapChainRenderer   (Windows, DXGI SwapChain)
///   - MetalRenderer            (macOS / iOS)
///   - VulkanRenderer           (Android / cross-platform)
class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    /// Initialize the renderer for the given frame dimensions.
    /// `device` is the platform GPU device (e.g. ID3D11Device*).
    virtual Result<void> Initialize(uint32_t width, uint32_t height,
                                    void* device) = 0;

    /// Render a decoded frame. The texture must be on the same GPU device.
    /// `native_texture` — platform GPU texture (e.g. ID3D11Texture2D*)
    /// `format` — pixel format of the input texture
    virtual Result<void> RenderFrame(void* native_texture,
                                     PixelFormat format) = 0;

    /// Present the rendered frame to the display.
    virtual Result<void> Present() = 0;

    /// Check whether the rendering window has been closed.
    virtual bool IsWindowClosed() const = 0;
};

}  // namespace lumen
