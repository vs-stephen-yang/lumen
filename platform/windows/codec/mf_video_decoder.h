#pragma once

#include "lumen/codec/video_decoder.h"
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

/// Hardware video decoder using Media Foundation Transform (MFT).
///
/// Mirrors MfVideoEncoder patterns. Supports both synchronous and asynchronous
/// MFTs. Outputs ID3D11Texture2D in NV12 format.
class MfVideoDecoder : public VideoDecoder, public IMFAsyncCallback {
public:
    explicit MfVideoDecoder(D3D11DeviceContext& device_ctx);
    ~MfVideoDecoder() override;

    Result<void> Initialize(const VideoDecoderConfig& config,
                            void* device) override;
    Result<DecodedFrame> Decode(const uint8_t* data, size_t size,
                                uint64_t frame_index) override;
    Result<void> Flush() override;
    void ReleaseFrame(const DecodedFrame& frame) override;
    VideoCodec GetCodec() const override;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFAsyncCallback
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override;
    STDMETHODIMP Invoke(IMFAsyncResult* result) override;

private:
    Result<void> FindHardwareDecoder(const GUID& subtype);
    Result<void> ConfigureDecoder(const VideoDecoderConfig& config);
    Result<void> SetInputMediaType(const VideoDecoderConfig& config);
    Result<void> SetOutputMediaType(const VideoDecoderConfig& config);

    Result<DecodedFrame> DecodeSyncMft(const uint8_t* data, size_t size,
                                       uint64_t frame_index);
    Result<DecodedFrame> DecodeAsyncMft(const uint8_t* data, size_t size,
                                        uint64_t frame_index);
    Result<DecodedFrame> DrainOutput(uint64_t frame_index);

    void ProcessAsyncOutput();
    void BeginEventLoop();

    ComPtr<IMFSample> CreateInputSample(const uint8_t* data, size_t size);

    D3D11DeviceContext& device_ctx_;
    ComPtr<IMFTransform> decoder_;
    ComPtr<IMFMediaEventGenerator> event_gen_;

    VideoDecoderConfig config_;
    std::atomic<ULONG> ref_count_{1};

    bool is_async_ = false;
    bool output_type_set_ = false;
    DWORD input_stream_id_ = 0;
    DWORD output_stream_id_ = 0;
    int64_t frame_duration_100ns_ = 0;
    int64_t timestamp_100ns_ = 0;

    // Standalone NV12 texture for CopyResource from decoder texture array.
    ComPtr<ID3D11Texture2D> staging_nv12_;

    // Async event signaling
    std::mutex async_mutex_;
    std::condition_variable input_ready_cv_;
    std::condition_variable output_ready_cv_;
    uint32_t input_needed_count_ = 0;
    bool output_available_ = false;
    bool drain_complete_ = false;
    DecodedFrame async_decoded_frame_;
    bool async_frame_ready_ = false;
};

}  // namespace lumen
