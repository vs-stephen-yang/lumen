#include "mf_video_decoder.h"
#include "../common/d3d11_device_context.h"

#include <mferror.h>

#include <sstream>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace lumen {

namespace {

GUID CodecToSubtype(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::kH264: return MFVideoFormat_H264;
        case VideoCodec::kHEVC: return MFVideoFormat_HEVC;
        case VideoCodec::kAV1:  return MFVideoFormat_AV1;
    }
    return MFVideoFormat_H264;
}

std::string HrToStr(HRESULT hr) {
    std::ostringstream oss;
    oss << "HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

STDMETHODIMP MfVideoDecoder::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MfVideoDecoder::AddRef() {
    return ref_count_.fetch_add(1) + 1;
}

STDMETHODIMP_(ULONG) MfVideoDecoder::Release() {
    ULONG count = ref_count_.fetch_sub(1) - 1;
    return count;
}

// ---------------------------------------------------------------------------
// IMFAsyncCallback
// ---------------------------------------------------------------------------

STDMETHODIMP MfVideoDecoder::GetParameters(DWORD* flags, DWORD* queue) {
    *flags = 0;
    *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
    return S_OK;
}

STDMETHODIMP MfVideoDecoder::Invoke(IMFAsyncResult* async_result) {
    ComPtr<IMFMediaEvent> event;
    HRESULT hr = event_gen_->EndGetEvent(async_result, &event);
    if (FAILED(hr)) return hr;

    MediaEventType event_type;
    event->GetType(&event_type);

    switch (event_type) {
        case METransformNeedInput: {
            std::lock_guard<std::mutex> lock(async_mutex_);
            input_needed_count_++;
            input_ready_cv_.notify_one();
            break;
        }

        case METransformHaveOutput: {
            ProcessAsyncOutput();
            break;
        }

        case METransformDrainComplete: {
            std::lock_guard<std::mutex> lock(async_mutex_);
            drain_complete_ = true;
            input_ready_cv_.notify_one();
            output_ready_cv_.notify_one();
            break;
        }

        default:
            break;
    }

    // Continue the event loop.
    {
        std::lock_guard<std::mutex> lock(async_mutex_);
        if (!drain_complete_) {
            event_gen_->BeginGetEvent(this, nullptr);
        }
    }

    return S_OK;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MfVideoDecoder::MfVideoDecoder(D3D11DeviceContext& device_ctx)
    : device_ctx_(device_ctx) {}

MfVideoDecoder::~MfVideoDecoder() {
    if (decoder_) {
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

Result<void> MfVideoDecoder::Initialize(const VideoDecoderConfig& config,
                                         void* /*device*/) {
    config_ = config;

    GUID subtype = CodecToSubtype(config.codec);
    auto result = FindHardwareDecoder(subtype);
    if (!result) return result;

    result = ConfigureDecoder(config);
    if (!result) return result;

    frame_duration_100ns_ = 10'000'000LL / config.fps;
    timestamp_100ns_ = 0;

    // Create a standalone NV12 texture for copying decoded frames.
    // Decoder output textures are in a texture array and lack SHADER_RESOURCE
    // bind flags, so we copy to this standalone texture for downstream use.
    auto tex_result = device_ctx_.CreateNV12Texture(config.width, config.height);
    if (!tex_result) return tex_result.error();
    staging_nv12_ = std::move(tex_result).value();

    return {};
}

Result<void> MfVideoDecoder::FindHardwareDecoder(const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO input_type = {};
    input_type.guidMajorType = MFMediaType_Video;
    input_type.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_type,
        nullptr,
        &activates,
        &count);

    if (FAILED(hr) || count == 0) {
        return Error::Make(ErrorCode::kUnsupported,
                           "No hardware video decoder found");
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&decoder_));

    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "Failed to activate hardware decoder MFT");
    }

    return {};
}

Result<void> MfVideoDecoder::ConfigureDecoder(const VideoDecoderConfig& config) {
    // Detect and unlock async MFTs.
    ComPtr<IMFAttributes> mft_attrs;
    HRESULT hr = decoder_->GetAttributes(&mft_attrs);
    if (SUCCEEDED(hr)) {
        UINT32 is_async = 0;
        mft_attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
        if (is_async) {
            mft_attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            is_async_ = true;
        }

        // Enable low-latency decoding.
        if (config.low_latency) {
            mft_attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        // Mark as D3D11-aware.
        mft_attrs->SetUINT32(MF_SA_D3D11_AWARE, TRUE);
    }

    // For async MFTs, get the event generator.
    if (is_async_) {
        hr = decoder_.As(&event_gen_);
        if (FAILED(hr)) {
            return Error::Make(ErrorCode::kDecoderError,
                               "Async MFT missing IMFMediaEventGenerator");
        }
    }

    // Get stream IDs.
    DWORD input_count = 0, output_count = 0;
    hr = decoder_->GetStreamCount(&input_count, &output_count);
    if (FAILED(hr) || input_count == 0 || output_count == 0) {
        return Error::Make(ErrorCode::kDecoderError, "GetStreamCount failed");
    }

    hr = decoder_->GetStreamIDs(1, &input_stream_id_, 1, &output_stream_id_);
    if (hr == E_NOTIMPL) {
        input_stream_id_ = 0;
        output_stream_id_ = 0;
    }

    // Low-latency via ICodecAPI.
    if (config.low_latency) {
        ComPtr<ICodecAPI> codec_api;
        if (SUCCEEDED(decoder_.As(&codec_api))) {
            VARIANT var;
            VariantInit(&var);
            var.vt = VT_BOOL;
            var.boolVal = VARIANT_TRUE;
            codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        }
    }

    // Set input type (compressed).
    auto result = SetInputMediaType(config);
    if (!result) return result;

    // Provide the DXGI device manager for hardware-accelerated decoding.
    decoder_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(device_ctx_.DXGIDeviceManager()));

    // Set output type (NV12 uncompressed).
    result = SetOutputMediaType(config);
    if (!result) return result;

    // Start streaming.
    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "NOTIFY_BEGIN_STREAMING failed: " + HrToStr(hr));
    }

    hr = decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "NOTIFY_START_OF_STREAM failed: " + HrToStr(hr));
    }

    // Start the async event loop.
    if (is_async_) {
        BeginEventLoop();
    }

    return {};
}

Result<void> MfVideoDecoder::SetInputMediaType(const VideoDecoderConfig& config) {
    GUID subtype = CodecToSubtype(config.codec);

    ComPtr<IMFMediaType> input_type;
    HRESULT hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) return Error::Make(ErrorCode::kDecoderError, "MFCreateMediaType failed");

    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = decoder_->SetInputType(input_stream_id_, input_type.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "SetInputType failed: " + HrToStr(hr));
    }

    return {};
}

Result<void> MfVideoDecoder::SetOutputMediaType(const VideoDecoderConfig& config) {
    // Enumerate available output types and prefer NV12.
    GUID output_subtype = MFVideoFormat_NV12;

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr = decoder_->GetOutputAvailableType(output_stream_id_, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID sub = {};
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);

        if (sub == MFVideoFormat_NV12) {
            output_subtype = sub;
            break;
        }
        if (i == 0) {
            output_subtype = sub;
        }
    }

    ComPtr<IMFMediaType> output_type;
    HRESULT hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) return Error::Make(ErrorCode::kDecoderError, "MFCreateMediaType failed");

    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, output_subtype);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = decoder_->SetOutputType(output_stream_id_, output_type.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "SetOutputType failed: " + HrToStr(hr));
    }

    output_type_set_ = true;
    return {};
}

// ---------------------------------------------------------------------------
// Async event loop
// ---------------------------------------------------------------------------

void MfVideoDecoder::BeginEventLoop() {
    event_gen_->BeginGetEvent(this, nullptr);
}

void MfVideoDecoder::ProcessAsyncOutput() {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output_data = {};
        output_data.dwStreamID = output_stream_id_;

        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &output_data, &status);

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Output type changed — renegotiate.
            // Search available types for NV12 (same pattern as SetOutputMediaType).
            ComPtr<IMFMediaType> nv12_type;
            ComPtr<IMFMediaType> fallback_type;
            for (DWORD i = 0; ; ++i) {
                ComPtr<IMFMediaType> candidate;
                HRESULT hr2 = decoder_->GetOutputAvailableType(
                    output_stream_id_, i, &candidate);
                if (hr2 == MF_E_NO_MORE_TYPES || FAILED(hr2)) break;

                GUID sub = {};
                candidate->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12) {
                    nv12_type = candidate;
                    break;
                }
                if (i == 0) {
                    fallback_type = candidate;
                }
            }

            ComPtr<IMFMediaType> chosen = nv12_type ? nv12_type : fallback_type;
            if (chosen) {
                decoder_->SetOutputType(output_stream_id_, chosen.Get(), 0);
                output_type_set_ = true;
            }
            if (output_data.pEvents) output_data.pEvents->Release();
            // Retry ProcessOutput to collect the pending frame.
            continue;
        }

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (output_data.pEvents) output_data.pEvents->Release();
            return;
        }

        if (SUCCEEDED(hr) && output_data.pSample) {
            ComPtr<IMFMediaBuffer> buffer;
            output_data.pSample->GetBufferByIndex(0, &buffer);

            ComPtr<IMFDXGIBuffer> dxgi_buffer;
            if (SUCCEEDED(buffer.As(&dxgi_buffer))) {
                ComPtr<ID3D11Texture2D> decoder_texture;
                UINT subresource_index = 0;
                dxgi_buffer->GetResource(IID_PPV_ARGS(&decoder_texture));
                dxgi_buffer->GetSubresourceIndex(&subresource_index);

                // Copy from decoder texture array slice to standalone texture.
                {
                    D3D11DeviceContext::ScopedLock lock(device_ctx_);
                    device_ctx_.Context()->CopySubresourceRegion(
                        staging_nv12_.Get(), 0, 0, 0, 0,
                        decoder_texture.Get(), subresource_index, nullptr);
                }

                std::lock_guard<std::mutex> lock(async_mutex_);
                async_decoded_frame_.native_texture = staging_nv12_.Get();
                async_decoded_frame_.subresource_index = 0;
                async_decoded_frame_.width = config_.width;
                async_decoded_frame_.height = config_.height;
                async_decoded_frame_.format = PixelFormat::kNV12;
                async_frame_ready_ = true;
                output_ready_cv_.notify_one();
            }
        }

        if (output_data.pEvents) output_data.pEvents->Release();
        return;
    }
}

// ---------------------------------------------------------------------------
// Input sample creation
// ---------------------------------------------------------------------------

ComPtr<IMFSample> MfVideoDecoder::CreateInputSample(const uint8_t* data, size_t size) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(hr)) return nullptr;

    BYTE* buf_data = nullptr;
    hr = buffer->Lock(&buf_data, nullptr, nullptr);
    if (FAILED(hr)) return nullptr;

    memcpy(buf_data, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return nullptr;

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(timestamp_100ns_);
    sample->SetSampleDuration(frame_duration_100ns_);
    timestamp_100ns_ += frame_duration_100ns_;

    return sample;
}

// ---------------------------------------------------------------------------
// Decode — dispatches to sync or async path
// ---------------------------------------------------------------------------

Result<DecodedFrame> MfVideoDecoder::Decode(const uint8_t* data, size_t size,
                                             uint64_t frame_index) {
    if (is_async_) {
        return DecodeAsyncMft(data, size, frame_index);
    }
    return DecodeSyncMft(data, size, frame_index);
}

// ---------------------------------------------------------------------------
// Synchronous MFT path
// ---------------------------------------------------------------------------

Result<DecodedFrame> MfVideoDecoder::DecodeSyncMft(const uint8_t* data, size_t size,
                                                    uint64_t frame_index) {
    auto sample = CreateInputSample(data, size);
    if (!sample) {
        return Error::Make(ErrorCode::kDecoderError, "Failed to create input sample");
    }

    HRESULT hr = decoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "ProcessInput failed: " + HrToStr(hr));
    }

    return DrainOutput(frame_index);
}

Result<DecodedFrame> MfVideoDecoder::DrainOutput(uint64_t frame_index) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output_data = {};
        output_data.dwStreamID = output_stream_id_;

        DWORD status = 0;
        HRESULT hr = decoder_->ProcessOutput(0, 1, &output_data, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // Decoder needs more data before it can produce output.
            return Error::Make(ErrorCode::kTimeout,
                               "Decoder needs more input");
        }

        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            // Output type changed — renegotiate and retry.
            ComPtr<IMFMediaType> new_output_type;
            hr = decoder_->GetOutputAvailableType(output_stream_id_, 0, &new_output_type);
            if (SUCCEEDED(hr)) {
                decoder_->SetOutputType(output_stream_id_, new_output_type.Get(), 0);
                output_type_set_ = true;
            }
            if (output_data.pEvents) output_data.pEvents->Release();
            continue;
        }

        if (FAILED(hr)) {
            if (output_data.pEvents) output_data.pEvents->Release();
            return Error::Make(ErrorCode::kDecoderError,
                               "ProcessOutput failed: " + HrToStr(hr));
        }

        IMFSample* result_sample = output_data.pSample;
        if (result_sample) {
            ComPtr<IMFMediaBuffer> buffer;
            result_sample->GetBufferByIndex(0, &buffer);

            ComPtr<IMFDXGIBuffer> dxgi_buffer;
            if (SUCCEEDED(buffer.As(&dxgi_buffer))) {
                ComPtr<ID3D11Texture2D> decoder_texture;
                UINT subresource_index = 0;
                dxgi_buffer->GetResource(IID_PPV_ARGS(&decoder_texture));
                dxgi_buffer->GetSubresourceIndex(&subresource_index);

                // Copy from decoder texture array slice to standalone texture.
                // Decoder textures lack SHADER_RESOURCE bind flag.
                {
                    D3D11DeviceContext::ScopedLock lock(device_ctx_);
                    device_ctx_.Context()->CopySubresourceRegion(
                        staging_nv12_.Get(), 0, 0, 0, 0,
                        decoder_texture.Get(), subresource_index, nullptr);
                }

                DecodedFrame frame;
                frame.native_texture = staging_nv12_.Get();
                frame.subresource_index = 0;
                frame.width = config_.width;
                frame.height = config_.height;
                frame.format = PixelFormat::kNV12;
                frame.frame_index = frame_index;

                if (output_data.pEvents) output_data.pEvents->Release();
                if (output_data.pSample) output_data.pSample->Release();
                return frame;
            }
        }

        if (output_data.pEvents) output_data.pEvents->Release();
        if (output_data.pSample) output_data.pSample->Release();
    }
}

// ---------------------------------------------------------------------------
// Async MFT path
// ---------------------------------------------------------------------------

Result<DecodedFrame> MfVideoDecoder::DecodeAsyncMft(const uint8_t* data, size_t size,
                                                     uint64_t frame_index) {
    auto sample = CreateInputSample(data, size);
    if (!sample) {
        return Error::Make(ErrorCode::kDecoderError, "Failed to create input sample");
    }

    // Wait for the MFT to signal it needs input.
    {
        std::unique_lock<std::mutex> lock(async_mutex_);
        bool got_signal = input_ready_cv_.wait_for(
            lock, std::chrono::seconds(5), [this] {
                return input_needed_count_ > 0 || drain_complete_;
            });

        if (!got_signal) {
            return Error::Make(ErrorCode::kTimeout,
                               "Timed out waiting for METransformNeedInput");
        }

        if (drain_complete_) {
            return Error::Make(ErrorCode::kDecoderError,
                               "Decoder drained unexpectedly");
        }

        input_needed_count_--;
        async_frame_ready_ = false;
    }

    HRESULT hr = decoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "ProcessInput failed: " + HrToStr(hr));
    }

    // Wait for output.
    {
        std::unique_lock<std::mutex> lock(async_mutex_);
        bool got_output = output_ready_cv_.wait_for(
            lock, std::chrono::seconds(5), [this] {
                return async_frame_ready_ || drain_complete_;
            });

        if (!got_output || !async_frame_ready_) {
            return Error::Make(ErrorCode::kTimeout,
                               "Decoder needs more input");
        }

        async_decoded_frame_.frame_index = frame_index;
        return async_decoded_frame_;
    }
}

// ---------------------------------------------------------------------------
// Flush / control
// ---------------------------------------------------------------------------

Result<void> MfVideoDecoder::Flush() {
    if (!decoder_) return {};

    HRESULT hr = decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kDecoderError,
                           "COMMAND_DRAIN failed: " + HrToStr(hr));
    }

    if (is_async_) {
        std::unique_lock<std::mutex> lock(async_mutex_);
        input_ready_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return drain_complete_;
        });
    }

    return {};
}

void MfVideoDecoder::ReleaseFrame(const DecodedFrame& /*frame*/) {
    // The staging texture is reused for each frame, so nothing to release.
    // In a more advanced implementation, this would return the texture to a pool.
}

VideoCodec MfVideoDecoder::GetCodec() const {
    return config_.codec;
}

}  // namespace lumen
