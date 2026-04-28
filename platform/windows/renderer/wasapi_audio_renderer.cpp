#include "wasapi_audio_renderer.h"

#include <Functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

namespace lumen {

// ─── IMMNotificationClient for device hot-plug ──────────────────────────────

class WasapiAudioRenderer::DeviceNotificationClient
    : public IMMNotificationClient {
public:
    explicit DeviceNotificationClient(std::atomic<bool>& device_changed)
        : device_changed_(device_changed) {}

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

    HRESULT STDMETHODCALLTYPE
    OnDefaultDeviceChanged(EDataFlow flow, ERole /*role*/,
                           LPCWSTR /*pwstrDeviceId*/) override {
        if (flow == eRender) {
            device_changed_.store(true, std::memory_order_release);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE
    OnDeviceAdded(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE
    OnDeviceRemoved(LPCWSTR /*pwstrDeviceId*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE
    OnDeviceStateChanged(LPCWSTR /*pwstrDeviceId*/,
                         DWORD /*dwNewState*/) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE
    OnPropertyValueChanged(LPCWSTR /*pwstrDeviceId*/,
                           const PROPERTYKEY /*key*/) override { return S_OK; }

private:
    std::atomic<bool>& device_changed_;
    ULONG ref_count_ = 1;
};

// ─── WasapiAudioRenderer ────────────────────────────────────────────────────

WasapiAudioRenderer::WasapiAudioRenderer() = default;

WasapiAudioRenderer::~WasapiAudioRenderer() {
    Stop();

    if (notification_client_ && enumerator_) {
        enumerator_->UnregisterEndpointNotificationCallback(
            notification_client_);
    }
    if (notification_client_) {
        notification_client_->Release();
        notification_client_ = nullptr;
    }

    ReleaseDevice();
    enumerator_.Reset();

    if (wasapi_event_) {
        CloseHandle(wasapi_event_);
        wasapi_event_ = nullptr;
    }
    if (stop_event_) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
    }
}

Result<void> WasapiAudioRenderer::Initialize(
    const AudioRendererConfig& config) {
    if (initialized_) {
        return Error::Make(ErrorCode::kAlreadyInitialized);
    }

    config_ = config;

    // Create events
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to create stop event");
    }

    wasapi_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wasapi_event_) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to create WASAPI event");
    }

    // Allocate ring buffer: capacity in total float samples (interleaved)
    size_t capacity_samples = static_cast<size_t>(config_.sample_rate) *
                              config_.channels * config_.buffer_capacity_ms /
                              1000;
    ring_buffer_ = std::make_unique<AudioRingBuffer>(capacity_samples);

    // COM init
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "COM initialization failed");
    }

    // Create device enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioRendererError,
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

Result<void> WasapiAudioRenderer::InitializeDevice() {
    ReleaseDevice();

    // Get default render endpoint
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(
        eRender, eConsole, device_.GetAddressOf());
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to get default audio endpoint");
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(audio_client_.GetAddressOf()));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to activate audio client");
    }

    // Build our desired format: 48kHz stereo float32 (Opus output)
    WAVEFORMATEX desired_format = {};
    desired_format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    desired_format.nChannels = static_cast<WORD>(config_.channels);
    desired_format.nSamplesPerSec = config_.sample_rate;
    desired_format.wBitsPerSample = 32;
    desired_format.nBlockAlign =
        desired_format.nChannels * desired_format.wBitsPerSample / 8;
    desired_format.nAvgBytesPerSec =
        desired_format.nSamplesPerSec * desired_format.nBlockAlign;
    desired_format.cbSize = 0;

    // Initialize with event-driven callback + auto format conversion
    DWORD stream_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                         AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                         AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    // Use 10ms buffer (system audio engine period for shared mode)
    REFERENCE_TIME buffer_duration = 100000;  // 10ms in 100ns units

    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags,
                                    buffer_duration, 0, &desired_format,
                                    nullptr);
    if (FAILED(hr)) {
        return Error::Make(
            ErrorCode::kAudioRendererError,
            "Failed to initialize audio client (hr=0x" +
                std::to_string(static_cast<unsigned long>(hr)) + ")");
    }

    // Set the event handle for event-driven rendering
    hr = audio_client_->SetEventHandle(wasapi_event_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to set event handle");
    }

    // Get the actual buffer size
    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to get buffer size");
    }

    // Get the render client
    hr = audio_client_->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(render_client_.GetAddressOf()));
    if (FAILED(hr)) {
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to get render client service");
    }

    device_changed_.store(false, std::memory_order_release);
    return {};
}

void WasapiAudioRenderer::ReleaseDevice() {
    render_client_.Reset();
    audio_client_.Reset();
    device_.Reset();
}

Result<void> WasapiAudioRenderer::Start() {
    if (!initialized_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }
    if (running_.load()) {
        return Error::Make(ErrorCode::kAlreadyInitialized,
                           "Renderer already running");
    }

    ResetEvent(stop_event_);
    pre_buffered_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    // Pre-fill the WASAPI endpoint buffer with silence so playback can start
    BYTE* data = nullptr;
    HRESULT hr = render_client_->GetBuffer(buffer_frame_count_, &data);
    if (SUCCEEDED(hr)) {
        render_client_->ReleaseBuffer(buffer_frame_count_,
                                       AUDCLNT_BUFFERFLAGS_SILENT);
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        running_.store(false);
        return Error::Make(ErrorCode::kAudioRendererError,
                           "Failed to start audio client");
    }

    render_thread_ = std::thread(&WasapiAudioRenderer::RenderLoop, this);
    return {};
}

Result<void> WasapiAudioRenderer::Stop() {
    if (!running_.load()) return {};

    running_.store(false, std::memory_order_release);
    SetEvent(stop_event_);

    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Reset();
    }

    return {};
}

Result<void> WasapiAudioRenderer::QueueFrame(const DecodedAudioFrame& frame) {
    if (!running_.load(std::memory_order_acquire)) {
        return Error::Make(ErrorCode::kNotInitialized, "Renderer not running");
    }

    size_t total_samples = frame.samples.size();
    if (total_samples == 0) return {};

    // Check for overrun
    if (ring_buffer_->WritableCount() < total_samples) {
        overrun_count_.fetch_add(1, std::memory_order_relaxed);
        return {};  // Drop the frame silently
    }

    ring_buffer_->Write(frame.samples.data(), total_samples);

    // Store the timestamp for this frame (latest queued)
    // The render loop will track the actual playback timestamp
    // based on consumed samples.
    // Simple approach: store timestamp and frame_count for mapping
    // We use the latest queued timestamp as a rough reference.
    // More precise tracking would need a timestamp FIFO.

    return {};
}

AudioRendererStats WasapiAudioRenderer::GetStats() const {
    AudioRendererStats stats;
    stats.frames_played = frames_played_.load(std::memory_order_relaxed);
    stats.underrun_count = underrun_count_.load(std::memory_order_relaxed);
    stats.overrun_count = overrun_count_.load(std::memory_order_relaxed);
    stats.total_underrun_samples =
        total_underrun_samples_.load(std::memory_order_relaxed);
    stats.buffer_level_ms = buffer_level_ms_.load(std::memory_order_relaxed);
    return stats;
}

Timestamp WasapiAudioRenderer::GetPlaybackTimestamp() const {
    return playback_timestamp_us_.load(std::memory_order_acquire);
}

void WasapiAudioRenderer::RenderLoop() {
    // MMCSS thread priority
    DWORD task_index = 0;
    HANDLE mmcss_handle =
        AvSetMmThreadCharacteristicsW(L"Audio", &task_index);

    // COM init for this thread
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    HANDLE wait_handles[2] = {stop_event_, wasapi_event_};

    // Pre-buffer threshold in total float samples (interleaved)
    size_t pre_buffer_threshold =
        static_cast<size_t>(config_.sample_rate) * config_.channels *
        config_.pre_buffer_ms / 1000;

    // EWMA smoothing factor for buffer level
    constexpr double kEwmaAlpha = 0.1;
    double ewma_level_ms = 0.0;

    // Track cumulative samples played for timestamp estimation
    uint64_t total_samples_played = 0;

    // Timestamp of the first queued frame (set by QueueFrame indirectly)
    // We approximate playback timestamp based on samples consumed.
    // The first frame's timestamp is captured when pre-buffering completes.
    Timestamp base_timestamp_us = 0;
    bool base_timestamp_set = false;

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

            // Pre-fill with silence and restart
            BYTE* data = nullptr;
            HRESULT hr =
                render_client_->GetBuffer(buffer_frame_count_, &data);
            if (SUCCEEDED(hr)) {
                render_client_->ReleaseBuffer(buffer_frame_count_,
                                               AUDCLNT_BUFFERFLAGS_SILENT);
            }
            audio_client_->Start();
        }

        // Wait for WASAPI buffer-ready or stop signal
        DWORD wait_result =
            WaitForMultipleObjects(2, wait_handles, FALSE, 100);
        if (wait_result == WAIT_OBJECT_0) break;  // stop_event signaled

        // Check pre-buffer threshold
        if (!pre_buffered_.load(std::memory_order_acquire)) {
            if (ring_buffer_->ReadableCount() < pre_buffer_threshold) {
                continue;  // Not enough data yet, output silence
            }
            pre_buffered_.store(true, std::memory_order_release);
        }

        // Get current padding (frames already in endpoint buffer)
        UINT32 padding = 0;
        HRESULT hr = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) continue;

        UINT32 available_frames = buffer_frame_count_ - padding;
        if (available_frames == 0) continue;

        UINT32 available_samples = available_frames * config_.channels;

        // Get WASAPI buffer
        BYTE* wasapi_data = nullptr;
        hr = render_client_->GetBuffer(available_frames, &wasapi_data);
        if (FAILED(hr)) continue;

        float* output = reinterpret_cast<float*>(wasapi_data);

        // Read from ring buffer
        size_t read = ring_buffer_->Read(output, available_samples);

        if (read < available_samples) {
            // Underrun: zero-fill the remainder
            std::memset(output + read, 0,
                        (available_samples - read) * sizeof(float));
            if (pre_buffered_.load(std::memory_order_relaxed)) {
                underrun_count_.fetch_add(1, std::memory_order_relaxed);
                total_underrun_samples_.fetch_add(
                    (available_samples - read) / config_.channels,
                    std::memory_order_relaxed);
            }
        }

        // Release buffer — always release full available_frames
        render_client_->ReleaseBuffer(available_frames, 0);

        UINT32 frames_written = static_cast<UINT32>(read / config_.channels);
        frames_played_.fetch_add(frames_written, std::memory_order_relaxed);
        total_samples_played += read;

        // Update EWMA buffer level
        size_t readable = ring_buffer_->ReadableCount();
        double current_level_ms =
            static_cast<double>(readable) /
            (config_.sample_rate * config_.channels) * 1000.0;
        ewma_level_ms =
            kEwmaAlpha * current_level_ms + (1.0 - kEwmaAlpha) * ewma_level_ms;
        buffer_level_ms_.store(ewma_level_ms, std::memory_order_relaxed);

        // Approximate playback timestamp:
        // We estimate based on total samples consumed at the output rate.
        // This is a rough approximation; precise tracking would need
        // per-frame timestamp correlation in the ring buffer.
        double seconds_played =
            static_cast<double>(total_samples_played) /
            (config_.sample_rate * config_.channels);
        if (!base_timestamp_set && read > 0) {
            // Use current time as base (will be refined with actual
            // frame timestamps when jitter buffer is implemented)
            base_timestamp_set = true;
        }
        playback_timestamp_us_.store(
            static_cast<int64_t>(seconds_played * 1'000'000.0),
            std::memory_order_release);
    }

    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }

    CoUninitialize();
}

}  // namespace lumen
