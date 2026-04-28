#pragma once

#include "lumen/capture/audio_capture.h"
#include "lumen/common/com_ptr.h"

#include <Audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <thread>

namespace lumen {

/// Audio capture implementation using WASAPI loopback mode.
///
/// Captures system audio output (what you hear) via the render endpoint
/// in loopback mode. Delivers interleaved float32 PCM at the system mix
/// format (expected 48kHz stereo on ~95% of systems).
///
/// Key details:
/// - Uses eRender + AUDCLNT_STREAMFLAGS_LOOPBACK (NOT eCapture)
/// - Polling loop with 10ms waits (loopback doesn't support event-driven)
/// - MMCSS "Pro Audio" thread priority for glitch-free capture
/// - Handles device hot-plug via IMMNotificationClient
class WasapiLoopbackCapture : public AudioCapture {
public:
    WasapiLoopbackCapture();
    ~WasapiLoopbackCapture() override;

    Result<void> Initialize() override;
    Result<void> Start(AudioFrameCallback callback) override;
    Result<void> Stop() override;
    AudioFormat GetFormat() const override;

private:
    Result<void> InitializeDevice();
    void CaptureLoop();

    // IMMNotificationClient implementation for device hot-plug
    class DeviceNotificationClient;

    ComPtr<IAudioClient> audio_client_;
    ComPtr<IAudioCaptureClient> capture_client_;
    ComPtr<IMMDevice> device_;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    DeviceNotificationClient* notification_client_ = nullptr;

    WAVEFORMATEX* mix_format_ = nullptr;
    AudioFormat format_;

    AudioFrameCallback callback_;
    std::thread capture_thread_;
    HANDLE stop_event_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> device_changed_{false};

    bool initialized_ = false;
};

}  // namespace lumen
