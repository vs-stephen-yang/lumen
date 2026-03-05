#pragma once

#include "lumen/codec/video_encoder.h"
#include "lumen/common/com_ptr.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace lumen {

class D3D11DeviceContext;

/// Hardware video encoder using Media Foundation Transform (MFT).
///
/// Supports both synchronous and asynchronous MFTs. Uses IMFAsyncCallback
/// for async MFTs (NVIDIA NVENC, Intel QSV, AMD VCE via MFT).
class MfVideoEncoder : public VideoEncoder, public IMFAsyncCallback {
public:
    explicit MfVideoEncoder(D3D11DeviceContext& device_ctx);
    ~MfVideoEncoder() override;

    Result<void> Initialize(const VideoEncoderConfig& config,
                            void* device) override;
    Result<void> Encode(void* native_texture,
                        const FrameMetadata& metadata) override;
    Result<void> Flush() override;
    void SetOutputCallback(EncodedPacketCallback callback) override;
    void RequestKeyframe() override;
    Result<void> SetBitrate(uint32_t bitrate_bps) override;
    Result<void> SetFrameRate(uint32_t fps) override;
    Result<void> Reconfigure(const VideoEncoderConfig& new_config) override;
    VideoCodec GetCodec() const override;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFAsyncCallback
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override;
    STDMETHODIMP Invoke(IMFAsyncResult* result) override;

private:
    Result<void> FindHardwareEncoder(const GUID& subtype);
    Result<void> ConfigureEncoder(const VideoEncoderConfig& config);
    Result<void> SetOutputMediaType(const VideoEncoderConfig& config);
    Result<void> SetInputMediaType(const VideoEncoderConfig& config);

    Result<void> EncodeSyncMft(void* native_texture, const FrameMetadata& metadata);
    Result<void> DrainOutput(const FrameMetadata& metadata);

    Result<void> EncodeAsyncMft(void* native_texture, const FrameMetadata& metadata);
    void ProcessAsyncOutput();
    void BeginEventLoop();

    ComPtr<IMFSample> CreateInputSample(void* native_texture);

    D3D11DeviceContext& device_ctx_;
    ComPtr<IMFTransform> encoder_;
    ComPtr<IMFMediaEventGenerator> event_gen_;
    ComPtr<ICodecAPI> codec_api_;

    VideoEncoderConfig config_;
    EncodedPacketCallback output_callback_;
    std::atomic<bool> keyframe_requested_{false};
    std::atomic<ULONG> ref_count_{1};

    bool is_async_ = false;
    DWORD input_stream_id_ = 0;
    DWORD output_stream_id_ = 0;
    int64_t frame_duration_100ns_ = 0;
    int64_t timestamp_100ns_ = 0;

    // Async event signaling
    std::mutex async_mutex_;
    std::condition_variable input_ready_cv_;
    uint32_t input_needed_count_ = 0;
    bool drain_complete_ = false;
    FrameMetadata current_metadata_;
};

}  // namespace lumen
