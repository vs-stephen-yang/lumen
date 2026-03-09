#pragma once

#include "lumen/codec/audio_decoder.h"
#include "lumen/common/error.h"
#include "lumen/common/types.h"

namespace lumen {

/// Abstract interface for audio playback/rendering.
///
/// Push model: caller pushes decoded audio frames via QueueFrame().
/// The renderer internally buffers samples and feeds them to the
/// platform audio output on its own schedule.
///
/// Implementations:
///   - WasapiAudioRenderer  (Windows, WASAPI shared-mode)
class AudioRenderer {
public:
    virtual ~AudioRenderer() = default;

    /// Initialize the renderer with the given configuration.
    virtual Result<void> Initialize(const AudioRendererConfig& config) = 0;

    /// Start audio playback. Pre-buffers before producing output.
    virtual Result<void> Start() = 0;

    /// Stop audio playback and release the render thread.
    virtual Result<void> Stop() = 0;

    /// Push a decoded audio frame into the renderer's ring buffer.
    /// Non-blocking. Drops the frame if the buffer is full (overrun).
    virtual Result<void> QueueFrame(const DecodedAudioFrame& frame) = 0;

    /// Get current playback statistics.
    virtual AudioRendererStats GetStats() const = 0;

    /// Get the timestamp (us) of the audio currently being output.
    /// Returns 0 if no audio has been played yet.
    virtual Timestamp GetPlaybackTimestamp() const = 0;
};

}  // namespace lumen
