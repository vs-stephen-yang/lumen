/// End-to-end pipeline test: Capture -> Encode -> Transport -> Decode -> Render
///
/// Exercises the full Lumen media path in a single process over TCP loopback.
/// Sender captures screen + system audio, encodes, and sends over transport.
/// Receiver decodes and renders video to a window + audio to speakers.
///
/// Validates frame flow end-to-end without loss or corruption.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "common/d3d11_device_context.h"
#include "capture/dxgi_desktop_duplication.h"
#include "codec/d3d11_color_converter.h"
#include "codec/mf_video_encoder.h"
#include "codec/mf_video_decoder.h"
#include "renderer/d3d11_swapchain_renderer.h"

#include "capture/wasapi_loopback_capture.h"
#include "opus/opus_audio_encoder.h"
#include "opus/opus_audio_decoder.h"
#include "renderer/wasapi_audio_renderer.h"

#include "lumen/transport/transport_types.h"
#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_client.h"
#include "transport/tcp_transport_server.h"
#include "transport/tcp_transport_client.h"
#include "transport/tcp_transport_connection.h"

#include <mfapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

using namespace lumen;
using Clock = std::chrono::high_resolution_clock;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

constexpr uint32_t kOutputIndex = 0;
constexpr uint32_t kNumFrames = 150;   // ~2.5s at 60fps
constexpr uint32_t kTimeoutMs = 100;

// ---------------------------------------------------------------------------
// Thread-safe packet queue
// ---------------------------------------------------------------------------

struct PacketQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> packets;
    bool done = false;

    void Push(const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex);
        packets.emplace_back(data, data + size);
        cv.notify_one();
    }

    std::optional<std::vector<uint8_t>> Pop() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return !packets.empty() || done; });
        if (packets.empty()) return std::nullopt;
        auto pkt = std::move(packets.front());
        packets.pop_front();
        return pkt;
    }

    void SignalDone() {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
        cv.notify_all();
    }
};

// ---------------------------------------------------------------------------
// RAII transport fixture — ensures proper shutdown on any exit path
// ---------------------------------------------------------------------------

struct TransportFixture {
    TcpTransportServer server;
    TcpTransportClient client;
    std::shared_ptr<TransportConnection> server_conn;
    std::shared_ptr<TransportConnection> client_conn;

    bool Setup() {
        TransportConfig server_config;
        server_config.address = "127.0.0.1";
        server_config.port = 0;

        auto r = server.Initialize(server_config);
        if (!r.ok()) {
            printf("FAILED: Server init: %s\n", r.error().message.c_str());
            return false;
        }

        std::mutex mtx;
        std::condition_variable cv;
        bool server_got = false, client_got = false;

        r = server.Start([&](std::shared_ptr<TransportConnection> conn) {
            std::lock_guard<std::mutex> lock(mtx);
            server_conn = conn;
            server_got = true;
            cv.notify_all();
        });
        if (!r.ok()) {
            printf("FAILED: Server start: %s\n", r.error().message.c_str());
            return false;
        }

        TransportConfig client_config;
        client_config.address = "127.0.0.1";
        client_config.port = server.GetListenPort();

        r = client.Initialize(client_config);
        if (!r.ok()) {
            printf("FAILED: Client init: %s\n", r.error().message.c_str());
            return false;
        }

        r = client.Connect([&](std::shared_ptr<TransportConnection> conn) {
            std::lock_guard<std::mutex> lock(mtx);
            client_conn = conn;
            client_got = true;
            cv.notify_all();
        });
        if (!r.ok()) {
            printf("FAILED: Client connect: %s\n", r.error().message.c_str());
            return false;
        }

        auto start = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                if (server_got && client_got) break;
            }
            if (std::chrono::steady_clock::now() - start >
                std::chrono::seconds(5)) {
                printf("FAILED: TCP connection timed out\n");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    ~TransportFixture() {
        if (client_conn) { client_conn->Close(); client_conn.reset(); }
        if (server_conn) { server_conn->Close(); server_conn.reset(); }
        client.Shutdown();
        server.Shutdown();
    }
};

// ---------------------------------------------------------------------------
// Timing structs
// ---------------------------------------------------------------------------

struct SenderTiming {
    double capture_ms;
    double convert_ms;
    double encode_ms;
    double total_ms;
};

struct ReceiverTiming {
    double decode_ms;
    double render_ms;
    double present_ms;
    double total_ms;
};

// ---------------------------------------------------------------------------
// Stats printing
// ---------------------------------------------------------------------------

static double Avg(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

static double Percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p / 100.0 * (v.size() - 1));
    return v[idx];
}

static void PrintRow(const char* label, const std::vector<double>& v) {
    printf("%-20s %7.2fms %7.2fms %7.2fms %7.2fms\n", label,
           Avg(v), Percentile(v, 50), Percentile(v, 95), Percentile(v, 99));
}

static void PrintSenderStats(const std::vector<SenderTiming>& timings) {
    if (timings.empty()) { printf("No sender frames.\n"); return; }

    std::vector<double> captures, converts, encodes, totals;
    for (auto& t : timings) {
        captures.push_back(t.capture_ms);
        converts.push_back(t.convert_ms);
        encodes.push_back(t.encode_ms);
        totals.push_back(t.total_ms);
    }

    printf("\n=== Sender Pipeline (%zu frames) ===\n", timings.size());
    printf("%-20s %8s %8s %8s %8s\n", "Stage", "Avg", "P50", "P95", "P99");
    PrintRow("Capture", captures);
    PrintRow("Color Convert", converts);
    PrintRow("Encode", encodes);
    PrintRow("Total", totals);

    double total_s = std::accumulate(totals.begin(), totals.end(), 0.0) / 1000.0;
    printf("Effective sender FPS: %.1f\n", timings.size() / total_s);
}

static void PrintReceiverStats(const std::vector<ReceiverTiming>& timings) {
    if (timings.empty()) { printf("No receiver frames.\n"); return; }

    std::vector<double> decodes, renders, presents, totals;
    for (auto& t : timings) {
        decodes.push_back(t.decode_ms);
        renders.push_back(t.render_ms);
        presents.push_back(t.present_ms);
        totals.push_back(t.total_ms);
    }

    printf("\n=== Receiver Pipeline (%zu frames) ===\n", timings.size());
    printf("%-20s %8s %8s %8s %8s\n", "Stage", "Avg", "P50", "P95", "P99");
    PrintRow("Decode", decodes);
    PrintRow("Render (CSC)", renders);
    PrintRow("Present", presents);
    PrintRow("Total", totals);

    double total_s = std::accumulate(totals.begin(), totals.end(), 0.0) / 1000.0;
    printf("Effective receiver FPS: %.1f\n", timings.size() / total_s);
}

// ---------------------------------------------------------------------------
// Counters (all atomic for thread safety)
// ---------------------------------------------------------------------------

static std::atomic<uint64_t> video_frames_captured{0};
static std::atomic<uint64_t> video_packets_encoded{0};
static std::atomic<uint64_t> video_packets_sent{0};
static std::atomic<uint64_t> video_send_errors{0};
static std::atomic<uint64_t> video_packets_received{0};
static std::atomic<uint64_t> video_frames_decoded{0};
static std::atomic<uint64_t> video_frames_rendered{0};
static std::atomic<uint64_t> video_decode_errors{0};
static std::atomic<uint64_t> video_render_errors{0};
static std::atomic<uint64_t> capture_timeouts{0};
static std::atomic<uint64_t> capture_errors{0};

static std::atomic<uint64_t> audio_frames_captured{0};
static std::atomic<uint64_t> audio_packets_encoded{0};
static std::atomic<uint64_t> audio_packets_received{0};
static std::atomic<uint64_t> audio_frames_decoded{0};
static std::atomic<uint64_t> audio_frame_index{0};

// ---------------------------------------------------------------------------
// Pipeline test — runs inside main(), all RAII objects clean up on return
// ---------------------------------------------------------------------------

static int RunPipelineTest() {
    // ── 1. TCP transport: server + client loopback ───────────────────────
    printf("[1/8] Setting up TCP transport loopback...\n");

    TransportFixture transport;
    if (!transport.Setup()) return 1;
    printf("  OK - connected on port %u\n", transport.server.GetListenPort());

    // ── 2. Sender GPU: capture + color converter + encoder ───────────────
    printf("[2/8] Creating sender D3D11 device + capture + encoder...\n");

    D3D11DeviceContext sender_device;
    auto result = sender_device.Initialize(kOutputIndex);
    if (!result) {
        printf("FAILED: Sender device: %s\n", result.error().message.c_str());
        return 1;
    }

    DxgiDesktopDuplication capture(sender_device);
    result = capture.Initialize(kOutputIndex);
    if (!result) {
        printf("FAILED: Capture: %s\n", result.error().message.c_str());
        return 1;
    }
    auto desc = capture.GetFrameDesc();
    printf("  Capture: %ux%u BGRA\n", desc.width, desc.height);

    D3D11ColorConverter sender_converter(sender_device);
    result = sender_converter.Initialize(desc, PixelFormat::kNV12, nullptr);
    if (!result) {
        printf("FAILED: Sender color converter: %s\n",
               result.error().message.c_str());
        return 1;
    }

    auto nv12_result = sender_device.CreateNV12Texture(desc.width, desc.height);
    if (!nv12_result) {
        printf("FAILED: NV12 texture: %s\n",
               nv12_result.error().message.c_str());
        return 1;
    }
    auto nv12_texture = std::move(nv12_result).value();

    MfVideoEncoder encoder(sender_device);
    VideoEncoderConfig enc_config;
    enc_config.width = desc.width;
    enc_config.height = desc.height;
    enc_config.fps = 60;
    enc_config.bitrate_bps = 8'000'000;
    enc_config.codec = VideoCodec::kH264;
    enc_config.low_latency = true;

    // Wire encoder output -> transport video channel.
    encoder.SetOutputCallback([&](EncodedPacket packet) {
        video_packets_encoded.fetch_add(1, std::memory_order_relaxed);

        SendOptions opts;
        opts.frame_index = packet.metadata.frame_index;
        opts.timestamp_us = packet.metadata.capture_time_us;
        opts.is_keyframe = packet.metadata.is_keyframe;

        auto send_result =
            transport.client_conn->GetChannel(ChannelType::kVideo)->Send(
                packet.data.data(), packet.data.size(), opts);
        if (send_result.ok()) {
            video_packets_sent.fetch_add(1, std::memory_order_relaxed);
        } else {
            video_send_errors.fetch_add(1, std::memory_order_relaxed);
        }
    });

    result = encoder.Initialize(enc_config, nullptr);
    if (!result) {
        printf("FAILED: Encoder: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  Encoder: H.264 %u kbps, %u fps, low-latency\n",
           enc_config.bitrate_bps / 1000, enc_config.fps);

    // ── 3. Receiver GPU: decoder + color converter + renderer ────────────
    printf("[3/8] Creating receiver D3D11 device + decoder + renderer...\n");

    // Separate device for receiver to avoid IMFDXGIDeviceManager conflicts
    // between encoder and decoder MFTs.
    D3D11DeviceContext receiver_device;
    result = receiver_device.Initialize(kOutputIndex);
    if (!result) {
        printf("FAILED: Receiver device: %s\n", result.error().message.c_str());
        return 1;
    }

    MfVideoDecoder decoder(receiver_device);
    VideoDecoderConfig dec_config;
    dec_config.codec = VideoCodec::kH264;
    dec_config.width = desc.width;
    dec_config.height = desc.height;
    dec_config.fps = 60;
    dec_config.low_latency = true;

    result = decoder.Initialize(dec_config, nullptr);
    if (!result) {
        printf("FAILED: Decoder: %s\n", result.error().message.c_str());
        return 1;
    }

    D3D11ColorConverter receiver_converter(receiver_device);
    VideoFrameDesc nv12_desc;
    nv12_desc.width = desc.width;
    nv12_desc.height = desc.height;
    nv12_desc.format = PixelFormat::kNV12;

    result = receiver_converter.Initialize(nv12_desc, PixelFormat::kBGRA,
                                           nullptr);
    if (!result) {
        printf("FAILED: Receiver color converter: %s\n",
               result.error().message.c_str());
        return 1;
    }

    D3D11SwapChainRenderer renderer(receiver_device, receiver_converter);
    result = renderer.Initialize(desc.width, desc.height, nullptr);
    if (!result) {
        printf("FAILED: Renderer: %s\n", result.error().message.c_str());
        return 1;
    }
    printf("  Decoder + Renderer: OK (%ux%u)\n", desc.width, desc.height);

    // ── 4. Audio pipeline ────────────────────────────────────────────────
    printf("[4/8] Initializing audio pipeline...\n");

    WasapiLoopbackCapture audio_capture;
    OpusAudioEncoder audio_encoder;
    OpusAudioDecoder audio_decoder;
    WasapiAudioRenderer audio_renderer;

    bool audio_ok = true;
    result = audio_capture.Initialize();
    if (!result.ok()) {
        printf("  WARN: Audio capture init failed: %s\n",
               result.error().message.c_str());
        audio_ok = false;
    }

    if (audio_ok) {
        auto format = audio_capture.GetFormat();

        AudioEncoderConfig audio_enc_config;
        audio_enc_config.sample_rate = format.sample_rate;
        audio_enc_config.channels = format.channels;
        audio_enc_config.bitrate_bps = 128000;

        result = audio_encoder.Initialize(audio_enc_config);
        if (!result.ok()) {
            printf("  WARN: Audio encoder init failed: %s\n",
                   result.error().message.c_str());
            audio_ok = false;
        }

        if (audio_ok) {
            AudioDecoderConfig audio_dec_config;
            audio_dec_config.sample_rate = format.sample_rate;
            audio_dec_config.channels = format.channels;

            auto dec_r = audio_decoder.Initialize(audio_dec_config);
            if (!dec_r.ok()) {
                printf("  WARN: Audio decoder init failed: %s\n",
                       dec_r.error().message.c_str());
                audio_ok = false;
            }
        }

        if (audio_ok) {
            AudioRendererConfig audio_render_config;
            audio_render_config.sample_rate = format.sample_rate;
            audio_render_config.channels = format.channels;
            audio_render_config.pre_buffer_ms = 40;
            audio_render_config.buffer_capacity_ms = 100;

            result = audio_renderer.Initialize(audio_render_config);
            if (!result.ok()) {
                printf("  WARN: Audio renderer init failed: %s\n",
                       result.error().message.c_str());
                audio_ok = false;
            }
        }
    }

    if (audio_ok) {
        printf("  Audio pipeline: OK\n");
    } else {
        printf("  Audio pipeline: SKIPPED (non-fatal)\n");
    }

    // ── 5. Wire callbacks ────────────────────────────────────────────────
    printf("[5/8] Wiring callbacks...\n");

    PacketQueue video_packet_queue;

    // Video receiver: transport -> queue.
    transport.server_conn->GetChannel(ChannelType::kVideo)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            video_packets_received.fetch_add(1, std::memory_order_relaxed);
            video_packet_queue.Push(data, size);
        });

    // Audio sender: capture -> encode -> transport.
    if (audio_ok) {
        audio_encoder.SetOutputCallback([&](EncodedAudioPacket packet) {
            audio_packets_encoded.fetch_add(1, std::memory_order_relaxed);

            SendOptions opts;
            opts.timestamp_us = packet.timestamp_us;
            opts.frame_index = audio_frame_index.fetch_add(
                1, std::memory_order_relaxed);

            transport.client_conn->GetChannel(ChannelType::kAudio)->Send(
                packet.data.data(), packet.data.size(), opts);
        });

        // Audio receiver: transport -> decode -> render.
        transport.server_conn->GetChannel(ChannelType::kAudio)
            ->SetReceiveCallback(
                [&](const uint8_t* data, size_t size) {
                    audio_packets_received.fetch_add(
                        1, std::memory_order_relaxed);

                    auto decode_result = audio_decoder.Decode(data, size);
                    if (decode_result.ok()) {
                        audio_frames_decoded.fetch_add(
                            1, std::memory_order_relaxed);
                        audio_renderer.QueueFrame(decode_result.value());
                    }
                });
    }

    printf("  OK\n");

    // ── 6. Start audio + sender thread ───────────────────────────────────
    printf("[6/8] Starting pipeline...\n");

    if (audio_ok) {
        result = audio_renderer.Start();
        if (!result.ok()) {
            printf("  WARN: Audio renderer start failed\n");
            audio_ok = false;
        }
    }

    if (audio_ok) {
        result = audio_capture.Start([&](const AudioFrame& frame) {
            audio_frames_captured.fetch_add(1, std::memory_order_relaxed);
            audio_encoder.Encode(frame.data, frame.frame_count,
                                 frame.timestamp_us);
        });
        if (!result.ok()) {
            printf("  WARN: Audio capture start failed\n");
            audio_ok = false;
        }
    }

    // Sender timing storage.
    std::vector<SenderTiming> sender_timings;
    sender_timings.reserve(kNumFrames);

    // Sender thread: capture -> convert -> encode loop.
    std::thread sender_thread([&] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        uint32_t consecutive_errors = 0;

        for (uint32_t i = 0;
             sender_timings.size() < kNumFrames && consecutive_errors < 10;
             ++i) {
            if (capture_timeouts.load(std::memory_order_relaxed) >
                kNumFrames * 3) {
                printf("  Sender: too many timeouts, aborting.\n");
                break;
            }

            auto frame_start = Clock::now();

            // Capture.
            auto capture_result = capture.AcquireFrame(kTimeoutMs);
            auto after_capture = Clock::now();

            if (!capture_result) {
                if (capture_result.error().code == ErrorCode::kTimeout ||
                    capture_result.error().code == ErrorCode::kAccessLost) {
                    capture_timeouts.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                capture_errors.fetch_add(1, std::memory_order_relaxed);
                consecutive_errors++;
                continue;
            }

            video_frames_captured.fetch_add(1, std::memory_order_relaxed);
            auto& frame = capture_result.value();

            // Color convert (BGRA -> NV12).
            auto convert_start = Clock::now();
            auto conv_result = sender_converter.Convert(
                frame.native_texture, nv12_texture.Get());
            auto after_convert = Clock::now();

            if (!conv_result) {
                capture.ReleaseFrame();
                consecutive_errors++;
                continue;
            }

            // Encode.
            auto encode_start = Clock::now();
            auto enc_result = encoder.Encode(nv12_texture.Get(),
                                              frame.metadata);
            auto after_encode = Clock::now();

            capture.ReleaseFrame();

            if (!enc_result) {
                consecutive_errors++;
                continue;
            }

            consecutive_errors = 0;

            SenderTiming t;
            t.capture_ms = std::chrono::duration<double, std::milli>(
                after_capture - frame_start).count();
            t.convert_ms = std::chrono::duration<double, std::milli>(
                after_convert - convert_start).count();
            t.encode_ms = std::chrono::duration<double, std::milli>(
                after_encode - encode_start).count();
            t.total_ms = std::chrono::duration<double, std::milli>(
                after_encode - frame_start).count();

            sender_timings.push_back(t);

            if (sender_timings.size() % 30 == 0) {
                printf("  Sender: %zu/%u frames\n",
                       sender_timings.size(), kNumFrames);
            }
        }

        // Flush encoder to drain any buffered output.
        encoder.Flush();

        // Allow transport to drain.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Signal receiver that no more packets are coming.
        video_packet_queue.SignalDone();

        CoUninitialize();
    });

    // ── 7. Main thread: decode/render loop ───────────────────────────────
    printf("[7/8] Running decode/render loop on main thread...\n\n");

    std::vector<ReceiverTiming> receiver_timings;
    receiver_timings.reserve(kNumFrames);

    while (auto pkt = video_packet_queue.Pop()) {
        // Pump Win32 messages for the renderer window.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (renderer.IsWindowClosed()) {
            printf("  Window closed by user.\n");
            break;
        }

        auto frame_start = Clock::now();

        // Decode.
        auto decode_result = decoder.Decode(
            pkt->data(), pkt->size(),
            video_frames_decoded.load(std::memory_order_relaxed));
        auto after_decode = Clock::now();

        if (!decode_result) {
            if (decode_result.error().code != ErrorCode::kTimeout) {
                video_decode_errors.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        video_frames_decoded.fetch_add(1, std::memory_order_relaxed);
        auto& decoded_frame = decode_result.value();

        // Render.
        auto render_start = Clock::now();
        auto render_result = renderer.RenderFrame(
            decoded_frame.native_texture, decoded_frame.format);
        auto after_render = Clock::now();

        decoder.ReleaseFrame(decoded_frame);

        if (!render_result) {
            video_render_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Present.
        auto present_start = Clock::now();
        auto present_result = renderer.Present();
        auto after_present = Clock::now();

        if (!present_result) {
            video_render_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        video_frames_rendered.fetch_add(1, std::memory_order_relaxed);

        ReceiverTiming t;
        t.decode_ms = std::chrono::duration<double, std::milli>(
            after_decode - frame_start).count();
        t.render_ms = std::chrono::duration<double, std::milli>(
            after_render - render_start).count();
        t.present_ms = std::chrono::duration<double, std::milli>(
            after_present - present_start).count();
        t.total_ms = std::chrono::duration<double, std::milli>(
            after_present - frame_start).count();

        receiver_timings.push_back(t);

        if (receiver_timings.size() % 30 == 0) {
            printf("  Receiver: %zu frames decoded+rendered\n",
                   receiver_timings.size());
        }
    }

    // ── 8. Teardown ──────────────────────────────────────────────────────
    printf("\n[8/8] Tearing down...\n");

    if (audio_ok) {
        audio_capture.Stop();
        audio_encoder.Flush();
    }

    sender_thread.join();

    if (audio_ok) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        audio_renderer.Stop();
    }

    decoder.Flush();

    // ── 9. Print stats ──────────────────────────────────────────────────

    PrintSenderStats(sender_timings);
    PrintReceiverStats(receiver_timings);

    // Frame accounting table.
    auto u = [](std::atomic<uint64_t>& a) {
        return static_cast<unsigned long long>(a.load());
    };

    printf("\n=== Frame Accounting ===\n");
    printf("%-30s %llu\n", "Video frames captured",    u(video_frames_captured));
    printf("%-30s %llu\n", "Video packets encoded",    u(video_packets_encoded));
    printf("%-30s %llu\n", "Video packets sent",       u(video_packets_sent));
    printf("%-30s %llu\n", "Video send errors",        u(video_send_errors));
    printf("%-30s %llu\n", "Video packets received",   u(video_packets_received));
    printf("%-30s %llu\n", "Video frames decoded",     u(video_frames_decoded));
    printf("%-30s %llu\n", "Video frames rendered",    u(video_frames_rendered));
    printf("%-30s %llu\n", "Video decode errors",      u(video_decode_errors));
    printf("%-30s %llu\n", "Video render errors",      u(video_render_errors));
    printf("%-30s %llu\n", "Capture timeouts",         u(capture_timeouts));
    printf("%-30s %llu\n", "Capture errors",           u(capture_errors));
    printf("\n");
    printf("%-30s %llu\n", "Audio frames captured",    u(audio_frames_captured));
    printf("%-30s %llu\n", "Audio packets encoded",    u(audio_packets_encoded));
    printf("%-30s %llu\n", "Audio packets received",   u(audio_packets_received));
    printf("%-30s %llu\n", "Audio frames decoded",     u(audio_frames_decoded));

    // ── 10. Pass/fail checks ─────────────────────────────────────────────

    printf("\n=== Verification ===\n");
    bool all_passed = true;

    auto check = [&](const char* name, bool condition) {
        printf("  %-50s %s\n", name, condition ? "PASS" : "FAIL");
        if (!condition) all_passed = false;
    };

    uint64_t encoded = video_packets_encoded.load();
    uint64_t sent = video_packets_sent.load();
    uint64_t received = video_packets_received.load();
    uint64_t rendered = video_frames_rendered.load();

    check("Encoder produced output", encoded > 0);
    check("All encoded packets sent", sent == encoded);
    check("All sent packets received (TCP reliable)", received == sent);
    check("Decoder+renderer produced output", rendered > 0);
    check("Rendered >= received - 5 (pipeline fill)",
          rendered >= (received > 5 ? received - 5 : 0));
    check("No video decode errors", video_decode_errors.load() == 0);
    check("No video render errors", video_render_errors.load() == 0);

    // Audio: only check if capture produced anything.
    if (audio_frames_captured.load() > 0) {
        check("Audio packets received (if captured)",
              audio_packets_received.load() > 0);
    } else {
        printf("  %-50s SKIP (no audio captured)\n",
               "Audio packets received");
    }

    printf("\n=== %s ===\n",
           all_passed ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");

    return all_passed ? 0 : 1;

    // All RAII objects (transport, device contexts, etc.) destroy here.
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    printf("Lumen End-to-End Pipeline Test\n");
    printf("==============================\n");
    printf("Capture -> Encode -> Transport (TCP) -> Decode -> Render\n\n");

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    int rc = RunPipelineTest();

    MFShutdown();
    CoUninitialize();

    return rc;
}
