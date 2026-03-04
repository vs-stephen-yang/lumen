#include "mf_video_encoder.h"
#include "../common/d3d11_device_context.h"

#include <mferror.h>
#include <wmcodecdsp.h>

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
        default: return MFVideoFormat_H264;
    }
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

STDMETHODIMP MfVideoEncoder::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
        *ppv = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MfVideoEncoder::AddRef() {
    return ref_count_.fetch_add(1) + 1;
}

STDMETHODIMP_(ULONG) MfVideoEncoder::Release() {
    ULONG count = ref_count_.fetch_sub(1) - 1;
    // Do NOT delete this; the object is stack/member-owned, not COM-allocated.
    return count;
}

// ---------------------------------------------------------------------------
// IMFAsyncCallback
// ---------------------------------------------------------------------------

STDMETHODIMP MfVideoEncoder::GetParameters(DWORD* flags, DWORD* queue) {
    // Return default parameters — use standard MF work queue.
    *flags = 0;
    *queue = MFASYNC_CALLBACK_QUEUE_MULTITHREADED;
    return S_OK;
}

STDMETHODIMP MfVideoEncoder::Invoke(IMFAsyncResult* async_result) {
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
            break;
        }

        default:
            break;
    }

    // Continue the event loop (unless drain is complete).
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

MfVideoEncoder::MfVideoEncoder(D3D11DeviceContext& device_ctx)
    : device_ctx_(device_ctx) {}

MfVideoEncoder::~MfVideoEncoder() {
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------

Result<void> MfVideoEncoder::Initialize(const VideoEncoderConfig& config,
                                         void* /*device*/) {
    config_ = config;

    GUID subtype = CodecToSubtype(config.codec);
    auto result = FindHardwareEncoder(subtype);
    if (!result) return result;

    result = ConfigureEncoder(config);
    if (!result) return result;

    frame_duration_100ns_ = 10'000'000LL / config.fps;
    timestamp_100ns_ = 0;

    return {};
}

Result<void> MfVideoEncoder::FindHardwareEncoder(const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO output_type = {};
    output_type.guidMajorType = MFMediaType_Video;
    output_type.guidSubtype = subtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr,
        &output_type,
        &activates,
        &count);

    if (FAILED(hr) || count == 0) {
        return Error::Make(ErrorCode::kUnsupported,
                           "No hardware video encoder found");
    }

    hr = activates[0]->ActivateObject(IID_PPV_ARGS(&encoder_));

    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "Failed to activate hardware encoder MFT");
    }

    return {};
}

Result<void> MfVideoEncoder::ConfigureEncoder(const VideoEncoderConfig& config) {
    // Detect and unlock async MFTs. Must be done before any other calls.
    ComPtr<IMFAttributes> mft_attrs;
    HRESULT hr = encoder_->GetAttributes(&mft_attrs);
    if (SUCCEEDED(hr)) {
        UINT32 is_async = 0;
        mft_attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async);
        if (is_async) {
            mft_attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            is_async_ = true;
        }
    }

    // For async MFTs, get the event generator.
    if (is_async_) {
        hr = encoder_.As(&event_gen_);
        if (FAILED(hr)) {
            return Error::Make(ErrorCode::kEncoderError,
                               "Async MFT missing IMFMediaEventGenerator");
        }
    }

    // Get stream IDs.
    DWORD input_count = 0, output_count = 0;
    hr = encoder_->GetStreamCount(&input_count, &output_count);
    if (FAILED(hr) || input_count == 0 || output_count == 0) {
        return Error::Make(ErrorCode::kEncoderError, "GetStreamCount failed");
    }

    hr = encoder_->GetStreamIDs(1, &input_stream_id_, 1, &output_stream_id_);
    if (hr == E_NOTIMPL) {
        input_stream_id_ = 0;
        output_stream_id_ = 0;
    }

    // Enable low-latency mode via ICodecAPI.
    ComPtr<ICodecAPI> codec_api;
    hr = encoder_.As(&codec_api);
    if (SUCCEEDED(hr)) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_BOOL;
        var.boolVal = VARIANT_TRUE;
        codec_api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
    }

    // Set output type (compressed).
    auto result = SetOutputMediaType(config);
    if (!result) return result;

    // Provide the DXGI device manager — must happen before input type enum.
    encoder_->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(device_ctx_.DXGIDeviceManager()));

    // Set input type (NV12 uncompressed).
    result = SetInputMediaType(config);
    if (!result) return result;

    // Set bitrate.
    if (codec_api) {
        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = config.bitrate_bps;
        codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    }

    // Start streaming.
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "NOTIFY_BEGIN_STREAMING failed: " + HrToStr(hr));
    }

    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "NOTIFY_START_OF_STREAM failed: " + HrToStr(hr));
    }

    // Start the async event loop after streaming has begun.
    if (is_async_) {
        BeginEventLoop();
    }

    return {};
}

Result<void> MfVideoEncoder::SetOutputMediaType(const VideoEncoderConfig& config) {
    GUID subtype = CodecToSubtype(config.codec);

    ComPtr<IMFMediaType> output_type;
    HRESULT hr = MFCreateMediaType(&output_type);
    if (FAILED(hr)) return Error::Make(ErrorCode::kEncoderError, "MFCreateMediaType failed");

    output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output_type->SetGUID(MF_MT_SUBTYPE, subtype);
    output_type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_bps);
    MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    if (subtype == MFVideoFormat_H264) {
        output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        output_type->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4_1);
    }

    hr = encoder_->SetOutputType(output_stream_id_, output_type.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "SetOutputType failed: " + HrToStr(hr));
    }

    return {};
}

Result<void> MfVideoEncoder::SetInputMediaType(const VideoEncoderConfig& config) {
    // Discover the MFT's preferred input subtype. Prefer NV12.
    GUID input_subtype = MFVideoFormat_NV12;

    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> candidate;
        HRESULT hr = encoder_->GetInputAvailableType(input_stream_id_, i, &candidate);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;

        GUID sub = {};
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);

        if (sub == MFVideoFormat_NV12) {
            input_subtype = sub;
            break;
        }
        if (i == 0) {
            input_subtype = sub;
        }
    }

    ComPtr<IMFMediaType> input_type;
    HRESULT hr = MFCreateMediaType(&input_type);
    if (FAILED(hr)) return Error::Make(ErrorCode::kEncoderError, "MFCreateMediaType failed");

    input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type->SetGUID(MF_MT_SUBTYPE, input_subtype);
    MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height);
    MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, config.fps, 1);
    input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = encoder_->SetInputType(input_stream_id_, input_type.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "SetInputType failed: " + HrToStr(hr));
    }

    return {};
}

// ---------------------------------------------------------------------------
// Async event loop
// ---------------------------------------------------------------------------

void MfVideoEncoder::BeginEventLoop() {
    // Kick off the async event pump. Invoke() will be called on an MF
    // work queue thread for each event.
    event_gen_->BeginGetEvent(this, nullptr);
}

void MfVideoEncoder::ProcessAsyncOutput() {
    MFT_OUTPUT_DATA_BUFFER output_data = {};
    output_data.dwStreamID = output_stream_id_;

    MFT_OUTPUT_STREAM_INFO stream_info = {};
    encoder_->GetOutputStreamInfo(output_stream_id_, &stream_info);

    ComPtr<IMFSample> output_sample;
    if (!(stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                  MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
        MFCreateSample(&output_sample);
        ComPtr<IMFMediaBuffer> out_buf;
        DWORD buf_size = stream_info.cbSize > 0 ? stream_info.cbSize : 1024 * 1024;
        MFCreateMemoryBuffer(buf_size, &out_buf);
        output_sample->AddBuffer(out_buf.Get());
        output_data.pSample = output_sample.Get();
    }

    DWORD status = 0;
    HRESULT hr = encoder_->ProcessOutput(0, 1, &output_data, &status);

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // Output type changed — renegotiate.
        ComPtr<IMFMediaType> new_output_type;
        hr = encoder_->GetOutputAvailableType(output_stream_id_, 0, &new_output_type);
        if (SUCCEEDED(hr)) {
            encoder_->SetOutputType(output_stream_id_, new_output_type.Get(), 0);
        }
        if (output_data.pEvents) output_data.pEvents->Release();
        return;
    }

    if (SUCCEEDED(hr) && output_data.pSample && output_callback_) {
        IMFSample* result_sample = output_data.pSample;
        ComPtr<IMFMediaBuffer> result_buf;
        result_sample->ConvertToContiguousBuffer(&result_buf);

        BYTE* data = nullptr;
        DWORD length = 0;
        result_buf->Lock(&data, nullptr, &length);

        EncodedPacket packet;
        packet.data.assign(data, data + length);

        // Copy metadata from the encoding thread.
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            packet.metadata = current_metadata_;
        }

        UINT32 clean_point = 0;
        if (SUCCEEDED(result_sample->GetUINT32(
                MFSampleExtension_CleanPoint, &clean_point))) {
            packet.metadata.is_keyframe = (clean_point != 0);
        }

        result_buf->Unlock();
        output_callback_(std::move(packet));
    }

    if (output_data.pEvents) {
        output_data.pEvents->Release();
    }
}

// ---------------------------------------------------------------------------
// Input sample creation
// ---------------------------------------------------------------------------

ComPtr<IMFSample> MfVideoEncoder::CreateInputSample(void* native_texture) {
    auto* texture = static_cast<ID3D11Texture2D*>(native_texture);

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
    if (FAILED(hr)) return nullptr;

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return nullptr;

    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(timestamp_100ns_);
    sample->SetSampleDuration(frame_duration_100ns_);
    timestamp_100ns_ += frame_duration_100ns_;

    if (keyframe_requested_.exchange(false)) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }

    return sample;
}

// ---------------------------------------------------------------------------
// Encode — dispatches to sync or async path
// ---------------------------------------------------------------------------

Result<void> MfVideoEncoder::Encode(void* native_texture,
                                     const FrameMetadata& metadata) {
    if (is_async_) {
        return EncodeAsyncMft(native_texture, metadata);
    }
    return EncodeSyncMft(native_texture, metadata);
}

// ---------------------------------------------------------------------------
// Synchronous MFT path
// ---------------------------------------------------------------------------

Result<void> MfVideoEncoder::EncodeSyncMft(void* native_texture,
                                            const FrameMetadata& metadata) {
    auto sample = CreateInputSample(native_texture);
    if (!sample) {
        return Error::Make(ErrorCode::kEncoderError, "Failed to create input sample");
    }

    HRESULT hr = encoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "ProcessInput failed: " + HrToStr(hr));
    }

    return DrainOutput(metadata);
}

Result<void> MfVideoEncoder::DrainOutput(const FrameMetadata& metadata) {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER output_data = {};
        output_data.dwStreamID = output_stream_id_;

        MFT_OUTPUT_STREAM_INFO stream_info = {};
        encoder_->GetOutputStreamInfo(output_stream_id_, &stream_info);

        ComPtr<IMFSample> output_sample;
        if (!(stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                      MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            MFCreateSample(&output_sample);
            ComPtr<IMFMediaBuffer> out_buf;
            DWORD buf_size = stream_info.cbSize > 0 ? stream_info.cbSize : 1024 * 1024;
            MFCreateMemoryBuffer(buf_size, &out_buf);
            output_sample->AddBuffer(out_buf.Get());
            output_data.pSample = output_sample.Get();
        }

        DWORD status = 0;
        HRESULT hr = encoder_->ProcessOutput(0, 1, &output_data, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;
        }

        if (FAILED(hr)) {
            if (output_data.pEvents) output_data.pEvents->Release();
            return Error::Make(ErrorCode::kEncoderError,
                               "ProcessOutput failed: " + HrToStr(hr));
        }

        IMFSample* result_sample = output_data.pSample;
        if (result_sample && output_callback_) {
            ComPtr<IMFMediaBuffer> result_buf;
            result_sample->ConvertToContiguousBuffer(&result_buf);

            BYTE* data = nullptr;
            DWORD length = 0;
            result_buf->Lock(&data, nullptr, &length);

            EncodedPacket packet;
            packet.data.assign(data, data + length);
            packet.metadata = metadata;

            UINT32 clean_point = 0;
            if (SUCCEEDED(result_sample->GetUINT32(
                    MFSampleExtension_CleanPoint, &clean_point))) {
                packet.metadata.is_keyframe = (clean_point != 0);
            }

            result_buf->Unlock();
            output_callback_(std::move(packet));
        }

        if (output_data.pEvents) {
            output_data.pEvents->Release();
        }
    }

    return {};
}

// ---------------------------------------------------------------------------
// Async MFT path — event-driven via IMFAsyncCallback
// ---------------------------------------------------------------------------

Result<void> MfVideoEncoder::EncodeAsyncMft(void* native_texture,
                                             const FrameMetadata& metadata) {
    auto sample = CreateInputSample(native_texture);
    if (!sample) {
        return Error::Make(ErrorCode::kEncoderError,
                           "Failed to create input sample");
    }

    // Store metadata for the async output callback.
    {
        std::lock_guard<std::mutex> lock(async_mutex_);
        current_metadata_ = metadata;
    }

    // Wait for the MFT to signal it needs input (METransformNeedInput).
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
            return Error::Make(ErrorCode::kEncoderError,
                               "Encoder drained unexpectedly");
        }

        input_needed_count_--;
    }

    HRESULT hr = encoder_->ProcessInput(input_stream_id_, sample.Get(), 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "ProcessInput failed: " + HrToStr(hr));
    }

    return {};
}

// ---------------------------------------------------------------------------
// Flush / control
// ---------------------------------------------------------------------------

Result<void> MfVideoEncoder::Flush() {
    if (!encoder_) return {};

    HRESULT hr = encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError,
                           "COMMAND_DRAIN failed: " + HrToStr(hr));
    }

    if (is_async_) {
        // Wait for METransformDrainComplete from the Invoke callback.
        std::unique_lock<std::mutex> lock(async_mutex_);
        input_ready_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
            return drain_complete_;
        });
    } else {
        FrameMetadata empty_meta;
        return DrainOutput(empty_meta);
    }

    return {};
}

void MfVideoEncoder::SetOutputCallback(EncodedPacketCallback callback) {
    output_callback_ = std::move(callback);
}

void MfVideoEncoder::RequestKeyframe() {
    keyframe_requested_ = true;
}

Result<void> MfVideoEncoder::SetBitrate(uint32_t bitrate_bps) {
    ComPtr<ICodecAPI> codec_api;
    HRESULT hr = encoder_.As(&codec_api);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kUnsupported, "ICodecAPI not available");
    }

    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = bitrate_bps;
    hr = codec_api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kEncoderError, "SetValue bitrate failed");
    }

    config_.bitrate_bps = bitrate_bps;
    return {};
}

VideoCodec MfVideoEncoder::GetCodec() const {
    return config_.codec;
}

}  // namespace lumen
