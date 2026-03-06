#pragma once

#include "lumen/common/error.h"
#include "lumen/common/types.h"

#include <functional>

namespace lumen {

/// Callback invoked when a new audio buffer is captured.
using AudioFrameCallback = std::function<void(const AudioFrame&)>;

/// Abstract interface for audio capture backends.
///
/// Implementations:
///   - WasapiLoopbackCapture  (Windows, system audio loopback)
///   - CoreAudioCapture       (macOS / iOS)
///   - AAudioCapture          (Android)
class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    /// Initialize the capture session.
    virtual Result<void> Initialize() = 0;

    /// Start capturing. Audio buffers are delivered via the callback.
    virtual Result<void> Start(AudioFrameCallback callback) = 0;

    /// Stop capturing and release the session.
    virtual Result<void> Stop() = 0;

    /// Query the current audio format (sample rate, channels, sample format).
    virtual AudioFormat GetFormat() const = 0;
};

}  // namespace lumen
