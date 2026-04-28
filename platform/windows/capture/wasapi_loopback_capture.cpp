#include "wasapi_loopback_capture.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <cstring>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace lumen {

// ─── IMMNotificationClient for device hot-plug ──────────────────────────────

class WasapiLoopbackCapture::DeviceNotificationClient
    : public IMMNotificationClient {
public:
    explicit DeviceNotificationClient(std::atomic<bool>& device_changed)
        : device_changed_(device_changed) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&ref_count_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = InterlockedDecrement(&ref_count_);
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                              void** ppvObject) override {
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE
    OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/,
                           LPCWSTR /*pwstrDeviceId*/) override {
        if (flow == eRender) {
            device_changed_.store(true, std::memory_order_release);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/,
                         DWORD /*dwNewState*/) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/,
                           const PROPERTYKEY /*key*/) override {
        return S_OK;
    }

private:
    std::atomic<bool>& device_changed_;
    ULONG ref_count_ = 1;
};

// ─── WasapiLoopbackCapture ──────────────────────────────────────────────────

WasapiLoopbackCapture::WasapiLoopbackCapture() = default;

WasapiLoopbackCapture::~WasapiLoopbackCapture() {
    Stop();

    if (notification_client_ && enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(
            notification_client_);
    }
    if (notification_client_) {
        notification_client_->Release();
        notification_client_ = nullptr;
    }
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }

    capture_client_.Reset();
    audio_client_.Reset();
    device_.Reset();
    enumerator_.Reset();

    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

Result<void> WasapiLoopbackCapture::Initialize() {
    if (initialized_) {
        return Error::Make(ErrorCode::kAlreadyInitialized);
    }

    // Create manual-reset stop event
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to create stop event");
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "COM initialization failed");
    }

    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to create device enumerator");
    }

    // Register for device change notifications
    notification_client_ = new DeviceNotificationClient(device_changed_);
    enumerator_->RegisterEndpointNotificationCallback(notification_client_);

    auto result = InitializeDevice();
    if (!result.ok()) return result;

    initialized_ = true;
    return {};
}

Result<void> WasapiLoopbackCapture::InitializeDevice() {
    // Release previous resources if re-initializing
    capture_client_.Reset();
    audio_client_.Reset();
    device_.Reset();
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }

    // Get default render endpoint (eRender for loopback, NOT eCapture)
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(
        eRender, eConsole, device_.GetAddressOf());
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to get default audio endpoint");
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(audio_client_.GetAddressOf()));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to activate audio client");
    }

    // Get the mix format
    hr = audio_client_->GetMixFormat(&mix_format_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to get mix format");
    }

    // Validate 48kHz — we don't implement a resampler
    if (mix_format_->nSamplesPerSec != 48000) {
        return Error::Make(
            ErrorCode::kUnsupported,
            "System mix format is not 48kHz (got " +
                std::to_string(mix_format_->nSamplesPerSec) +
                "Hz). Resampling not implemented.");
    }

    // Populate our format struct
    format_.sample_rate = mix_format_->nSamplesPerSec;
    format_.channels = mix_format_->nChannels;
    format_.sample_format = AudioSampleFormat::kFloat32;

    // Initialize audio client in shared loopback mode
    // Use 20ms buffer (matching Opus frame size)
    REFERENCE_TIME buffer_duration = 200000;  // 20ms in 100ns units
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    buffer_duration, 0, mix_format_, nullptr);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to initialize audio client for loopback");
    }

    hr = audio_client_->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(capture_client_.GetAddressOf()));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to get capture client service");
    }

    device_changed_.store(false, std::memory_order_release);
    return {};
}

Result<void> WasapiLoopbackCapture::Start(AudioFrameCallback callback) {
    if (!initialized_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }
    if (running_.load()) {
        return Error::Make(ErrorCode::kAlreadyInitialized,
                           "Capture already running");
    }

    callback_ = std::move(callback);
    ResetEvent(stop_event_);
    running_.store(true, std::memory_order_release);

    HRESULT hr = audio_client_->Start();
    if (FAILED(hr)) {
        running_.store(false);
        return Error::Make(ErrorCode::kAudioCaptureError,
                           "Failed to start audio client");
    }

    capture_thread_ = std::thread(&WasapiLoopbackCapture::CaptureLoop, this);
    return {};
}

Result<void> WasapiLoopbackCapture::Stop() {
    if (!running_.load()) return {};

    running_.store(false, std::memory_order_release);
    SetEvent(stop_event_);

    if (capture_thread_.joinable()) {
        capture_thread_.join();
    }

    if (audio_client_) {
        audio_client_->Stop();
    }

    callback_ = nullptr;
    return {};
}

AudioFormat WasapiLoopbackCapture::GetFormat() const { return format_; }

void WasapiLoopbackCapture::CaptureLoop() {
    // Boost thread priority via MMCSS
    DWORD task_index = 0;
    HANDLE mmcss_handle =
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    // COM init for this thread
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (running_.load(std::memory_order_acquire)) {
        // Check for device hot-plug
        if (device_changed_.load(std::memory_order_acquire)) {
            audio_client_->Stop();
            auto result = InitializeDevice();
            if (!result.ok()) {
                // Device re-init failed, wait and retry
                WaitForSingleObject(stop_event_, 100);
                continue;
            }
            audio_client_->Start();
        }

        // Wait 10ms or until stop is signaled
        DWORD wait_result = WaitForSingleObject(stop_event_, 10);
        if (wait_result == WAIT_OBJECT_0) break;

        // Drain all available packets
        UINT32 packet_length = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&packet_length);
        if (FAILED(hr)) continue;

        while (packet_length > 0) {
            BYTE* data = nullptr;
            UINT32 num_frames = 0;
            DWORD flags = 0;
            UINT64 device_position = 0;
            UINT64 qpc_position = 0;

            hr = capture_client_->GetBuffer(&data, &num_frames, &flags,
                                             &device_position, &qpc_position);
            if (FAILED(hr)) break;

            if (callback_ && num_frames > 0) {
                AudioFrame frame;
                frame.data = reinterpret_cast<const float*>(data);
                frame.frame_count = num_frames;
                frame.channels = format_.channels;
                frame.silent =
                    (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                // Convert QPC (100ns units) to microseconds
                if (qpc_position > 0) {
                    frame.timestamp_us =
                        static_cast<Timestamp>(qpc_position / 10);
                }

                callback_(frame);
            }

            capture_client_->ReleaseBuffer(num_frames);

            hr = capture_client_->GetNextPacketSize(&packet_length);
            if (FAILED(hr)) break;
        }
    }

    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }

    CoUninitialize();
}

}  // namespace lumen
