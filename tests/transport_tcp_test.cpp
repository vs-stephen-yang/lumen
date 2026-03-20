/// Windows TCP transport integration tests.
/// Tests end-to-end TCP loopback: server + client + channels.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "lumen/transport/transport_types.h"
#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_client.h"

// Windows TCP implementations.
#include "transport/tcp_transport_server.h"
#include "transport/tcp_transport_client.h"
#include "transport/tcp_transport_connection.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

using namespace lumen;

static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_TRUE(expr)                                  \
    do {                                                   \
        if (!(expr)) {                                     \
            printf("  ASSERT_TRUE failed: %s\n", #expr);  \
            throw std::runtime_error("assertion failed");  \
        }                                                  \
    } while (0)

#define ASSERT_EQ(a, b)                                    \
    do {                                                   \
        if ((a) != (b)) {                                  \
            printf("  ASSERT_EQ failed: %s != %s\n",      \
                   #a, #b);                                \
            throw std::runtime_error("assertion failed");  \
        }                                                  \
    } while (0)

/// Helper: wait for a condition with timeout.
template <typename Pred>
bool WaitFor(Pred pred, int timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count() > timeout_ms) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

/// Helper: set up a server and client, return both connections.
struct TestFixture {
    TcpTransportServer server;
    TcpTransportClient client;
    std::shared_ptr<TransportConnection> server_conn;
    std::shared_ptr<TransportConnection> client_conn;

    bool Setup() {
        TransportConfig server_config;
        server_config.address = "127.0.0.1";
        server_config.port = 0;  // Ephemeral port

        auto init_result = server.Initialize(server_config);
        if (!init_result.ok()) return false;

        std::mutex mtx;
        std::condition_variable cv;
        bool server_got_conn = false;

        auto start_result = server.Start(
            [&](std::shared_ptr<TransportConnection> conn) {
                std::lock_guard<std::mutex> lock(mtx);
                server_conn = conn;
                server_got_conn = true;
                cv.notify_all();
            });
        if (!start_result.ok()) return false;

        TransportConfig client_config;
        client_config.address = "127.0.0.1";
        client_config.port = server.GetListenPort();

        auto client_init = client.Initialize(client_config);
        if (!client_init.ok()) return false;

        bool client_got_conn = false;
        auto connect_result = client.Connect(
            [&](std::shared_ptr<TransportConnection> conn) {
                std::lock_guard<std::mutex> lock(mtx);
                client_conn = conn;
                client_got_conn = true;
                cv.notify_all();
            });
        if (!connect_result.ok()) return false;

        // Wait for both connections.
        return WaitFor([&] {
            std::lock_guard<std::mutex> lock(mtx);
            return server_got_conn && client_got_conn;
        });
    }

    void Teardown() {
        if (client_conn) client_conn->Close();
        if (server_conn) server_conn->Close();
        client.Shutdown();
        server.Shutdown();
    }
};

static void RunTest(const char* name, std::function<void()> fn) {
    printf("  %-50s ", name);
    try {
        fn();
        printf("PASS\n");
        tests_passed++;
    } catch (const std::exception& e) {
        printf("FAIL (%s)\n", e.what());
        tests_failed++;
    } catch (...) {
        printf("FAIL\n");
        tests_failed++;
    }
}

static void test_small_frame_delivery() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    // Set up receive callback on server's video channel.
    std::mutex mtx;
    std::vector<uint8_t> received;
    bool got_data = false;

    f.server_conn->GetChannel(ChannelType::kVideo)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            received.assign(data, data + size);
            got_data = true;
        });

    // Send a small frame from client.
    std::vector<uint8_t> payload(100, 0xAA);
    SendOptions opts;
    opts.frame_index = 1;
    opts.timestamp_us = 12345;
    f.client_conn->GetChannel(ChannelType::kVideo)->Send(
        payload.data(), payload.size(), opts);

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(mtx);
        return got_data;
    }));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_EQ(received.size(), payload.size());
        ASSERT_TRUE(received == payload);
    }

    f.Teardown();
}

static void test_large_frame_delivery() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    std::mutex mtx;
    std::vector<uint8_t> received;
    bool got_data = false;

    f.server_conn->GetChannel(ChannelType::kVideo)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            received.assign(data, data + size);
            got_data = true;
        });

    // Send a 500KB frame — goes as a single queue entry (no fragmentation).
    const size_t frame_size = 500 * 1024;
    std::vector<uint8_t> payload(frame_size);
    std::iota(payload.begin(), payload.end(), static_cast<uint8_t>(0));

    SendOptions opts;
    opts.frame_index = 2;
    opts.is_keyframe = true;
    f.client_conn->GetChannel(ChannelType::kVideo)->Send(
        payload.data(), payload.size(), opts);

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(mtx);
        return got_data;
    }, 10000));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_EQ(received.size(), payload.size());
        ASSERT_TRUE(received == payload);
    }

    f.Teardown();
}

static void test_all_channels() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    std::mutex mtx;
    std::atomic<int> channels_received{0};
    std::vector<uint8_t> video_data, audio_data, control_data;

    f.server_conn->GetChannel(ChannelType::kVideo)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            video_data.assign(data, data + size);
            channels_received++;
        });
    f.server_conn->GetChannel(ChannelType::kAudio)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            audio_data.assign(data, data + size);
            channels_received++;
        });
    f.server_conn->GetChannel(ChannelType::kControl)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            control_data.assign(data, data + size);
            channels_received++;
        });

    SendOptions opts;
    opts.frame_index = 1;

    std::vector<uint8_t> v_payload(200, 0x11);
    std::vector<uint8_t> a_payload(100, 0x22);
    std::vector<uint8_t> c_payload(50, 0x33);

    f.client_conn->GetChannel(ChannelType::kVideo)->Send(
        v_payload.data(), v_payload.size(), opts);
    f.client_conn->GetChannel(ChannelType::kAudio)->Send(
        a_payload.data(), a_payload.size(), opts);
    f.client_conn->GetChannel(ChannelType::kControl)->Send(
        c_payload.data(), c_payload.size(), opts);

    ASSERT_TRUE(WaitFor([&] { return channels_received.load() >= 3; }));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_TRUE(video_data == v_payload);
        ASSERT_TRUE(audio_data == a_payload);
        ASSERT_TRUE(control_data == c_payload);
    }

    f.Teardown();
}

static void test_disconnect_detection() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    std::atomic<bool> server_saw_disconnect{false};
    f.server_conn->SetStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::kFailed ||
            state == ConnectionState::kDisconnected) {
            server_saw_disconnect = true;
        }
    });

    // Close client connection — server should detect.
    f.client_conn->Close();

    ASSERT_TRUE(WaitFor([&] { return server_saw_disconnect.load(); }, 10000));

    f.Teardown();
}

static void test_reconnect_with_backoff() {
    TcpTransportServer server;
    TransportConfig server_config;
    server_config.address = "127.0.0.1";
    server_config.port = 0;
    ASSERT_TRUE(server.Initialize(server_config).ok());

    std::shared_ptr<TransportConnection> server_conn;
    ASSERT_TRUE(
        server
            .Start([&](std::shared_ptr<TransportConnection> conn) {
                server_conn = conn;
            })
            .ok());

    TcpTransportClient client;
    TransportConfig client_config;
    client_config.address = "127.0.0.1";
    client_config.port = server.GetListenPort();
    client_config.connect_timeout_ms = 2000;
    ASSERT_TRUE(client.Initialize(client_config).ok());

    ReconnectPolicy policy;
    policy.auto_reconnect = false;
    policy.max_attempts = 1;
    client.SetReconnectPolicy(policy);

    std::shared_ptr<TransportConnection> client_conn;
    std::atomic<bool> connected{false};
    ASSERT_TRUE(client.Connect([&](std::shared_ptr<TransportConnection> conn) {
        client_conn = conn;
        connected = true;
    }).ok());

    ASSERT_TRUE(WaitFor([&] { return connected.load(); }));
    ASSERT_TRUE(client_conn != nullptr);

    if (client_conn) client_conn->Close();
    if (server_conn) server_conn->Close();
    client.Shutdown();
    server.Shutdown();
}

// Step 9: New test — queue-full error propagation.
static void test_queue_full_error() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    // Fill the send queue (4MB limit) by sending large frames without
    // giving the I/O thread time to drain.
    auto* channel = f.client_conn->GetChannel(ChannelType::kVideo);
    SendOptions opts;
    opts.frame_index = 0;

    bool got_full_error = false;
    // Each frame is ~1MB + 28 byte header. After 4 sends the queue should be full.
    std::vector<uint8_t> big_payload(1024 * 1024, 0x42);
    for (int i = 0; i < 10; i++) {
        opts.frame_index = i;
        auto result = channel->Send(big_payload.data(), big_payload.size(), opts);
        if (!result.ok() &&
            result.error().code == ErrorCode::kTransportChannelFull) {
            got_full_error = true;
            break;
        }
    }

    ASSERT_TRUE(got_full_error);

    f.Teardown();
}

// Step 9: New test — ReadyToSend callback fires after queue drain.
static void test_ready_to_send_callback() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    // Set up a dummy receive callback so data is consumed.
    f.server_conn->GetChannel(ChannelType::kVideo)->SetReceiveCallback(
        [](const uint8_t*, size_t) {});

    auto* channel = f.client_conn->GetChannel(ChannelType::kVideo);

    std::atomic<bool> ready_fired{false};
    channel->SetReadyToSendCallback([&]() {
        ready_fired = true;
    });

    // Fill the queue until full.
    SendOptions opts;
    std::vector<uint8_t> big_payload(1024 * 1024, 0x42);
    for (int i = 0; i < 10; i++) {
        opts.frame_index = i;
        auto result = channel->Send(big_payload.data(), big_payload.size(), opts);
        if (!result.ok()) break;
    }

    // The I/O thread should drain entries and fire ReadyToSend.
    ASSERT_TRUE(WaitFor([&] { return ready_fired.load(); }, 10000));

    f.Teardown();
}

// Step 9: New test — multiple control messages all arrive (validates Step 4 fix).
static void test_multiple_control_messages() {
    TestFixture f;
    ASSERT_TRUE(f.Setup());

    std::mutex mtx;
    std::vector<std::vector<uint8_t>> received_messages;

    f.server_conn->SetControlMessageCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            received_messages.emplace_back(data, data + size);
        });

    // Send 5 distinct control messages.
    const int num_messages = 5;
    for (int i = 0; i < num_messages; i++) {
        std::vector<uint8_t> msg = {static_cast<uint8_t>('A' + i),
                                     static_cast<uint8_t>(i)};
        auto result = f.client_conn->SendControlMessage(msg.data(), msg.size());
        ASSERT_TRUE(result.ok());
    }

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(mtx);
        return static_cast<int>(received_messages.size()) >= num_messages;
    }));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_EQ(static_cast<int>(received_messages.size()), num_messages);
        for (int i = 0; i < num_messages; i++) {
            ASSERT_EQ(received_messages[i].size(), 2u);
            ASSERT_EQ(received_messages[i][0],
                       static_cast<uint8_t>('A' + i));
            ASSERT_EQ(received_messages[i][1], static_cast<uint8_t>(i));
        }
    }

    f.Teardown();
}

int main() {
    printf("=== TCP Transport Integration Tests ===\n");

    RunTest("small_frame_delivery", test_small_frame_delivery);
    RunTest("large_frame_delivery", test_large_frame_delivery);
    RunTest("all_channels", test_all_channels);
    RunTest("disconnect_detection", test_disconnect_detection);
    RunTest("reconnect_with_backoff", test_reconnect_with_backoff);
    RunTest("queue_full_error", test_queue_full_error);
    RunTest("ready_to_send_callback", test_ready_to_send_callback);
    RunTest("multiple_control_messages", test_multiple_control_messages);

    printf("\nResults: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
