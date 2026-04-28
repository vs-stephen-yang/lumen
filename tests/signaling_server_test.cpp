// Integration test for SignalingServer.
//
// Spins up a real SignalingServer on an ephemeral port, hits it with
// httplib::Client, and asserts on the JSON body and pickup behaviour.

#include "lumen/signaling/signaling_server.h"
#include "lumen/signaling/session_config.h"

#include <httplib.h>

#include <chrono>
#include <cstdio>
#include <string>

using namespace lumen;
using namespace std::chrono_literals;

namespace {
int passed = 0;
int failed = 0;

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg);     \
                   ++failed; return; }                                         \
} while (0)

void test_session_endpoint_returns_configured_json() {
    SignalingServer server;
    auto init = server.Initialize(0, "");
    CHECK(init.ok(), "Initialize");
    auto start = server.Start();
    CHECK(start.ok(), "Start");
    const uint16_t port = server.GetPort();
    CHECK(port != 0, "port should be non-zero");

    SessionConfig cfg;
    cfg.session_id = "abc-123";
    cfg.transport.url = "ws://localhost:18443/lumen";
    cfg.transport.ssrc = 0xDEADBEEF;
    cfg.video.width = 1280;
    cfg.video.height = 720;
    cfg.video.fps = 30;
    cfg.video.bitrate = 4'000'000;
    server.SetSessionConfig(cfg);

    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/api/session");
    CHECK(res, "GET /api/session must succeed");
    CHECK(res->status == 200, "status 200");

    const std::string& body = res->body;
    CHECK(body.find("\"session_id\":\"abc-123\"") != std::string::npos,
          "session_id present");
    CHECK(body.find("\"url\":\"ws://localhost:18443/lumen\"") != std::string::npos,
          "transport.url present");
    CHECK(body.find("\"ssrc\":3735928559") != std::string::npos,
          "ssrc decimal-encoded");
    CHECK(body.find("\"width\":1280") != std::string::npos, "video.width");
    CHECK(body.find("\"fps\":30") != std::string::npos, "video.fps");

    server.Stop();
    ++passed;
}

void test_session_endpoint_503_when_unconfigured() {
    SignalingServer server;
    server.Initialize(0, "");
    server.Start();

    httplib::Client cli("127.0.0.1", server.GetPort());
    auto res = cli.Get("/api/session");
    CHECK(res, "request");
    CHECK(res->status == 503, "503 when no session set");

    server.Stop();
    ++passed;
}

void test_wait_for_session_pickup_unblocks_on_fetch() {
    SignalingServer server;
    server.Initialize(0, "");
    server.Start();

    SessionConfig cfg;
    cfg.session_id = "wait-test";
    cfg.transport.url = "ws://localhost:18443/lumen";
    server.SetSessionConfig(cfg);

    // Pickup hasn't happened yet — short timeout should fail.
    auto early = server.WaitForSessionPickup(50ms);
    CHECK(!early.ok(), "should time out before any GET");

    // Now hit the endpoint and confirm pickup unblocks.
    std::thread fetcher([port = server.GetPort()] {
        httplib::Client cli("127.0.0.1", port);
        cli.Get("/api/session");
    });

    auto picked = server.WaitForSessionPickup(2000ms);
    fetcher.join();
    CHECK(picked.ok(), "pickup should be observed");
    CHECK(picked.value().session_id == "wait-test", "config matches");

    server.Stop();
    ++passed;
}

void test_root_redirects_to_sender() {
    SignalingServer server;
    server.Initialize(0, "");
    server.Start();

    httplib::Client cli("127.0.0.1", server.GetPort());
    cli.set_follow_location(false);
    auto res = cli.Get("/");
    CHECK(res, "GET /");
    CHECK(res->status == 302, "302 redirect");
    CHECK(res->get_header_value("Location") == "/sender/", "Location header");

    server.Stop();
    ++passed;
}

}  // namespace

int main() {
    test_session_endpoint_returns_configured_json();
    test_session_endpoint_503_when_unconfigured();
    test_wait_for_session_pickup_unblocks_on_fetch();
    test_root_redirects_to_sender();

    std::printf("signaling_server_test: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
