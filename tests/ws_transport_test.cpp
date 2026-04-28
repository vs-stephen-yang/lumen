// Loopback integration test for WsTransportServer.
//
// Spins up the server, connects a TCP client, performs an HTTP/1.1 → WS
// upgrade by hand, and exchanges a binary message. Asserts that the
// channel-dispatch path delivers the payload bytes intact.

#include "transport/ws_frame_codec.h"
#include "transport/ws_handshake.h"
#include "transport/ws_transport_server.h"

#include "lumen/transport/transport_channel.h"
#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

using namespace lumen;
using namespace std::chrono_literals;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond, msg) do {                                                  \
    if (!(cond)) { std::fprintf(stderr, "FAIL [%s]: %s\n", __func__, msg);     \
                   ++failed; return; }                                         \
} while (0)

bool RecvAll(SOCKET s, char* buf, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, buf + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool SendAll(SOCKET s, const void* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, static_cast<const char*>(data) + sent,
                     static_cast<int>(len - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

bool ReadHttpResponse(SOCKET s, std::string& out) {
    out.clear();
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, buf + n);
        if (out.size() > 16 * 1024) return false;
    }
    return true;
}

SOCKET ConnectLocal(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

void test_handshake_and_binary_send_to_video_channel() {
    WsTransportServer server;
    TransportConfig cfg;
    cfg.address = "0.0.0.0";
    cfg.port = 0;  // ephemeral
    auto init = server.Initialize(cfg);
    CHECK(init.ok(), "server.Initialize");

    std::shared_ptr<TransportConnection> server_conn;
    std::mutex conn_mu;
    std::condition_variable conn_cv;
    auto start = server.Start([&](std::shared_ptr<TransportConnection> c) {
        std::lock_guard<std::mutex> lock(conn_mu);
        server_conn = std::move(c);
        conn_cv.notify_all();
    });
    CHECK(start.ok(), "server.Start");

    const uint16_t port = server.GetListenPort();
    CHECK(port != 0, "ephemeral port");

    SOCKET client = ConnectLocal(port);
    CHECK(client != INVALID_SOCKET, "client connect");

    // Send the HTTP/1.1 upgrade by hand.
    const std::string req =
        "GET /lumen HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    CHECK(SendAll(client, req.data(), req.size()), "send upgrade req");

    std::string response;
    CHECK(ReadHttpResponse(client, response), "read upgrade response");
    CHECK(response.find("HTTP/1.1 101") == 0, "101 status");
    CHECK(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
              std::string::npos,
          "RFC 6455 §1.3 expected accept value");

    // Wait for the server to surface the connection.
    {
        std::unique_lock<std::mutex> lock(conn_mu);
        conn_cv.wait_for(lock, 2s, [&] { return server_conn != nullptr; });
    }
    CHECK(server_conn != nullptr, "server fired incoming callback");

    // Wire a receive callback on the video channel.
    std::vector<uint8_t> received;
    std::atomic<bool> got{false};
    auto* video = server_conn->GetChannel(ChannelType::kVideo);
    CHECK(video != nullptr, "video channel exists");
    video->SetReceiveCallback([&](const uint8_t* d, size_t n) {
        received.assign(d, d + n);
        got.store(true);
    });

    // Build a message body: [channel_id=video][payload]
    const std::vector<uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF, 0x42, 0x00, 0xFF};
    std::vector<uint8_t> body;
    body.push_back(static_cast<uint8_t>(ChannelType::kVideo));
    body.insert(body.end(), payload.begin(), payload.end());

    auto frame = WriteFrame(WsOpcode::kBinary, body.data(), body.size(),
                             /*mask=*/true, /*mask_key_be=*/0xCAFEBABE);
    CHECK(SendAll(client, frame.data(), frame.size()), "send WS frame");

    // Wait for the channel callback to fire.
    for (int i = 0; i < 200 && !got.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(got.load(), "video receive callback fired");
    CHECK(received == payload, "payload bytes match");

    // Cleanly close
    auto close_frame = WriteFrame(WsOpcode::kClose, nullptr, 0, true, 0x12345678);
    SendAll(client, close_frame.data(), close_frame.size());
    closesocket(client);

    server_conn->Close();
    server.Shutdown();
    ++passed;
}

void test_bad_request_rejected() {
    WsTransportServer server;
    TransportConfig cfg;
    cfg.port = 0;
    server.Initialize(cfg);
    server.Start([](std::shared_ptr<TransportConnection>) {});

    SOCKET client = ConnectLocal(server.GetListenPort());
    CHECK(client != INVALID_SOCKET, "connect");

    // Plain HTTP, no upgrade headers.
    const std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    SendAll(client, req.data(), req.size());

    std::string resp;
    ReadHttpResponse(client, resp);
    CHECK(resp.find("HTTP/1.1 400") == 0, "400 Bad Request");

    closesocket(client);
    server.Shutdown();
    ++passed;
}

void test_compute_accept_key_rfc_example() {
    // RFC 6455 §1.3 example.
    auto v = ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
    CHECK(v == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "RFC 6455 example accept key");
    ++passed;
}

}  // namespace

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    test_compute_accept_key_rfc_example();
    test_handshake_and_binary_send_to_video_channel();
    test_bad_request_rejected();

    WSACleanup();
    std::printf("ws_transport_test: %d passed, %d failed\n",
                passed, failed);
    return failed == 0 ? 0 : 1;
}
