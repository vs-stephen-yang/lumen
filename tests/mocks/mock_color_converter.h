#pragma once

#include "lumen/codec/color_converter.h"

#include <cstdint>
#include <optional>

namespace lumen {

/// Mock color converter for unit testing. Records Convert() calls and supports
/// error injection; performs no actual pixel work (textures are opaque here).
class MockColorConverter : public ColorConverter {
public:
    Result<void> Initialize(const VideoFrameDesc& input_desc,
                            PixelFormat output_format, void* /*device*/) override {
        if (init_error_) return Error::Make(*init_error_, "mock init");
        input_desc_ = input_desc;
        output_format_ = output_format;
        initialized_ = true;
        return {};
    }

    Result<void> Convert(void* /*input_texture*/,
                         void* /*output_texture*/) override {
        convert_count_++;
        if (convert_error_) return Error::Make(*convert_error_, "mock convert");
        return {};
    }

    PixelFormat GetOutputFormat() const override { return output_format_; }

    // ── Test controls ───────────────────────────────────────────────
    void FailInitializeWith(ErrorCode c) { init_error_ = c; }
    void FailConvertWith(ErrorCode c) { convert_error_ = c; }

    bool IsInitialized() const { return initialized_; }
    uint32_t ConvertCount() const { return convert_count_; }

private:
    bool initialized_ = false;
    VideoFrameDesc input_desc_;
    PixelFormat output_format_ = PixelFormat::kNV12;
    uint32_t convert_count_ = 0;
    std::optional<ErrorCode> init_error_;
    std::optional<ErrorCode> convert_error_;
};

}  // namespace lumen
