#pragma once

#include "lumen/renderer/audio_renderer.h"
#include "lumen/common/com_ptr.h"
#include "audio_ring_buffer.h"

#include <Audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <memory>
#include <thread>

namespace lumen {

/// WASAPI shared-mode audio renderer.
///
/// Event-driven render thread feeds decoded PCM audio from an internal
/// ring buffer to the default audio endpoint. Handles device hot-plug,
/// pre-buffering, underrun/overrun tracking, and playback timestamp.
class WasapiAudioRenderer : public AudioRenderer {
public:
    WasapiAudioRenderer();
    ~WasapiAudioRenderer() override;

    Result<void> Initialize(const AudioRendererConfig& config) override;
    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> QueueFrame(const DecodedAudioFrame& frame) override;
    AudioRendererStats GetStats() const override;
    Timestamp GetPlaybackTimestamp() const override;

private:
    Result<void> InitializeDevice();
    void ReleaseDevice();
    void RenderLoop();

    class DeviceNotificationClient;

    AudioRendererConfig config_;
    std::unique_ptr<AudioRingBuffer> ring_buffer_;

    // WASAPI COM objects
    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioRenderClient> render_client_;
    ComPtr<IMMDevice> device_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    DeviceNotificationClient* notification_client_ = nullptr;

    UINT32 buffer_frame_count_ = 0;  // WASAPI endpoint buffer size in frames

    // Events
    HANDLE wasapi_event_ = nullptr;  // WASAPI buffer-ready event
    HANDLE stop_event_ = nullptr;    // Shutdown signal

    // Render thread
    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> device_changed_{false};
    std::atomic<bool> pre_buffered_{false};

    // Statistics (atomics for cross-thread reads)
    std::atomic<uint64_t> frames_played_{0};
    std::atomic<uint64_t> underrun_count_{0};
    std::atomic<uint64_t> overrun_count_{0};
    std::atomic<uint64_t> total_underrun_samples_{0};

    // Playback timestamp tracking
    std::atomic<int64_t> playback_timestamp_us_{0};

    // EWMA buffer level for drift instrumentation
    std::atomic<double> buffer_level_ms_{0.0};

    bool initialized_ = false;
};

}  // namespace lumen
