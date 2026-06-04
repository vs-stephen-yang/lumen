#pragma once

#include "lumen/renderer/video_renderer.h"

#include <cstdint>
#include <optional>

namespace lumen {

/// Mock video renderer for unit testing. Headless-friendly (opens no window),
/// records RenderFrame()/Present() calls, and supports error injection plus a
/// settable window-closed flag.
class MockVideoRenderer : public VideoRenderer {
public:
    Result<void> Initialize(uint32_t width, uint32_t height,
                            void* /*device*/) override {
        if (init_error_) return Error::Make(*init_error_, "mock init");
        width_ = width;
        height_ = height;
        initialized_ = true;
        return {};
    }

    Result<void> RenderFrame(void* /*native_texture*/,
                             PixelFormat /*format*/) override {
        render_count_++;
        if (render_error_) return Error::Make(*render_error_, "mock render");
        return {};
    }

    Result<void> Present() override {
        present_count_++;
        if (present_error_) return Error::Make(*present_error_, "mock present");
        return {};
    }

    bool IsWindowClosed() const override { return window_closed_; }

    // ── Test controls ───────────────────────────────────────────────
    void FailInitializeWith(ErrorCode c) { init_error_ = c; }
    void FailRenderWith(ErrorCode c) { render_error_ = c; }
    void FailPresentWith(ErrorCode c) { present_error_ = c; }
    void SetWindowClosed(bool v) { window_closed_ = v; }

    bool IsInitialized() const { return initialized_; }
    uint32_t RenderCount() const { return render_count_; }
    uint32_t PresentCount() const { return present_count_; }

private:
    bool initialized_ = false;
    bool window_closed_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t render_count_ = 0;
    uint32_t present_count_ = 0;
    std::optional<ErrorCode> init_error_;
    std::optional<ErrorCode> render_error_;
    std::optional<ErrorCode> present_error_;
};

}  // namespace lumen
