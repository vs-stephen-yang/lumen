#include "lumen/signaling/signaling_server.h"

#include <httplib.h>

#include <atomic>
#include <sstream>

namespace lumen {

namespace {

std::string EscapeJson(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

std::string SerializeSession(const SessionConfig& s) {
    std::ostringstream o;
    o << "{"
      << "\"session_id\":\""    << EscapeJson(s.session_id)            << "\","
      << "\"transport\":{"
      <<   "\"kind\":\""        << EscapeJson(s.transport.kind)        << "\","
      <<   "\"url\":\""         << EscapeJson(s.transport.url)         << "\","
      <<   "\"ssrc\":"          << s.transport.ssrc
      << "},"
      << "\"video\":{"
      <<   "\"codec\":\""       << EscapeJson(s.video.codec)           << "\","
      <<   "\"width\":"         << s.video.width                       << ","
      <<   "\"height\":"        << s.video.height                      << ","
      <<   "\"fps\":"           << s.video.fps                         << ","
      <<   "\"bitrate\":"       << s.video.bitrate
      << "},"
      << "\"audio\":{"
      <<   "\"codec\":\""       << EscapeJson(s.audio.codec)           << "\","
      <<   "\"sample_rate\":"   << s.audio.sample_rate                 << ","
      <<   "\"channels\":"      << s.audio.channels                    << ","
      <<   "\"bitrate\":"       << s.audio.bitrate
      << "}"
      << "}";
    return o.str();
}

}  // namespace

struct SignalingServer::Impl {
    httplib::Server http;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<uint16_t> bound_port{0};

    std::mutex mu;
    SessionConfig current_config;
    bool config_set = false;
    bool picked_up = false;
    SessionConfig picked_up_config;
    std::condition_variable cv;
};

SignalingServer::SignalingServer() : impl_(std::make_unique<Impl>()) {}

SignalingServer::~SignalingServer() { Stop(); }

Result<void> SignalingServer::Initialize(uint16_t port,
                                          const std::string& static_root) {
    impl_->http.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/sender/", 302);
    });

    if (!static_root.empty()) {
        // Mount static_root at /sender. cpp-httplib serves the directory
        // contents directly; index.html is found automatically.
        if (!impl_->http.set_mount_point("/sender", static_root)) {
            return Error::Make(ErrorCode::kInvalidArgument,
                               "static_root not found: " + static_root);
        }
    }

    impl_->http.Get("/api/session",
                    [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->config_set) {
            res.status = 503;
            res.set_content("{\"error\":\"no session configured\"}",
                            "application/json");
            return;
        }
        res.set_content(SerializeSession(impl_->current_config),
                        "application/json");
        res.set_header("Cache-Control", "no-store");

        if (!impl_->picked_up) {
            impl_->picked_up = true;
            impl_->picked_up_config = impl_->current_config;
            impl_->cv.notify_all();
        }
    });

    // Bind without listening yet. cpp-httplib has two distinct APIs:
    //   - bind_to_port(host, port)         → bool, requires non-zero port
    //   - bind_to_any_port(host)           → int, returns the chosen port
    // Use the right one depending on whether the caller asked for an
    // ephemeral port.
    int actual_port = 0;
    if (port == 0) {
        actual_port = impl_->http.bind_to_any_port("0.0.0.0");
        if (actual_port <= 0) {
            return Error::Make(ErrorCode::kTransportError,
                               "Failed to bind signaling server on any port");
        }
    } else {
        if (!impl_->http.bind_to_port("0.0.0.0", port)) {
            return Error::Make(ErrorCode::kTransportError,
                               "Failed to bind signaling server on port " +
                                   std::to_string(port));
        }
        actual_port = port;
    }
    impl_->bound_port.store(static_cast<uint16_t>(actual_port));

    return {};
}

Result<void> SignalingServer::Start() {
    if (impl_->bound_port.load() == 0) {
        return Error::Make(ErrorCode::kNotInitialized,
                           "Initialize() not called");
    }
    if (impl_->running.exchange(true)) {
        return Error::Make(ErrorCode::kAlreadyInitialized,
                           "Signaling server already running");
    }

    impl_->thread = std::thread([this] {
        impl_->http.listen_after_bind();
    });

    // Wait until the listen loop is actually accepting before returning,
    // so callers (and tests) don't race against an un-listening socket.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (!impl_->http.is_running()) {
        if (std::chrono::steady_clock::now() > deadline) {
            impl_->http.stop();
            if (impl_->thread.joinable()) impl_->thread.join();
            impl_->running.store(false);
            return Error::Make(ErrorCode::kTransportError,
                               "Signaling server failed to start listening");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    return {};
}

void SignalingServer::Stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->http.stop();
    if (impl_->thread.joinable()) impl_->thread.join();

    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->cv.notify_all();
    }
}

void SignalingServer::SetSessionConfig(const SessionConfig& config) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->current_config = config;
    impl_->config_set = true;
    impl_->picked_up = false;
}

Result<SessionConfig> SignalingServer::WaitForSessionPickup(
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(impl_->mu);
    if (!impl_->cv.wait_for(lock, timeout,
                             [this] { return impl_->picked_up ||
                                             !impl_->running.load(); })) {
        return Error::Make(ErrorCode::kTimeout,
                           "No sender fetched /api/session in time");
    }
    if (!impl_->picked_up) {
        return Error::Make(ErrorCode::kNotInitialized, "Server stopped");
    }
    return impl_->picked_up_config;
}

uint16_t SignalingServer::GetPort() const {
    return impl_->bound_port.load();
}

}  // namespace lumen
