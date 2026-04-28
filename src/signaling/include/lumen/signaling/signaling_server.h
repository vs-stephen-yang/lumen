#pragma once

#include "lumen/common/error.h"
#include "lumen/signaling/session_config.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace lumen {

/// Tiny HTTP signaling server.
///
/// Serves the static web sender bundle and exposes a single JSON endpoint
/// (`/api/session`) that returns the current SessionConfig. No bidirectional
/// signaling — the web sender pulls config once and then opens the transport
/// URL it was given.
///
/// Implementation details (cpp-httplib) are hidden in the .cpp; the header
/// only depends on the public types so callers don't pull in httplib.
class SignalingServer {
public:
    SignalingServer();
    ~SignalingServer();

    /// Bind the HTTP listener and prepare the static file root.
    /// `static_root` may be empty to disable static file serving.
    Result<void> Initialize(uint16_t port, const std::string& static_root);

    /// Start the HTTP server thread. Non-blocking.
    Result<void> Start();

    /// Stop the server and join the thread.
    void Stop();

    /// Update the session config returned by /api/session.
    void SetSessionConfig(const SessionConfig& config);

    /// Block until the sender has fetched /api/session, or timeout.
    Result<SessionConfig> WaitForSessionPickup(
        std::chrono::milliseconds timeout);

    /// Port the server is actually bound to (useful when port=0 was passed).
    uint16_t GetPort() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lumen
