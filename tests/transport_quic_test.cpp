/// Windows QUIC transport integration tests.
/// Tests end-to-end QUIC loopback using msquic: server + client + channels.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")

#include "lumen/transport/transport_types.h"
#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_client.h"

// Windows QUIC implementations.
#include "transport/quic_transport_server.h"
#include "transport/quic_transport_client.h"

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
static int tests_skipped = 0;

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

/// Thrown when a test cannot run due to platform limitations.
struct test_skipped : std::exception {
    const char* what() const noexcept override { return "skipped"; }
};

// ---------------------------------------------------------------------------
// Self-signed certificate helper (Windows CryptoAPI)
// ---------------------------------------------------------------------------

static const wchar_t* kTestKeyName = L"LumenQuicTestKey";

/// Holds a self-signed test certificate and its SHA1 thumbprint.
struct TestCert {
    PCCERT_CONTEXT context = nullptr;
    QUIC_CERTIFICATE_HASH hash = {};
    bool valid = false;
};

/// Create a self-signed certificate using CNG, add it to the CurrentUser\MY
/// store so Schannel can find the private key. Returns the cert hash for
/// QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH.
static TestCert CreateSelfSignedTestCert() {
    TestCert result;

    // 1. Create a CNG RSA key.
    NCRYPT_PROV_HANDLE hProv = 0;
    if (FAILED(NCryptOpenStorageProvider(&hProv, MS_KEY_STORAGE_PROVIDER, 0))) {
        return result;
    }

    // Delete any stale key from a prior run.
    NCRYPT_KEY_HANDLE hOld = 0;
    if (SUCCEEDED(NCryptOpenKey(hProv, &hOld, kTestKeyName, 0, 0))) {
        NCryptDeleteKey(hOld, 0);
    }

    NCRYPT_KEY_HANDLE hKey = 0;
    if (FAILED(NCryptCreatePersistedKey(hProv, &hKey,
                                         BCRYPT_ECDSA_P256_ALGORITHM,
                                         kTestKeyName, 0,
                                         NCRYPT_OVERWRITE_KEY_FLAG))) {
        NCryptFreeObject(hProv);
        return result;
    }

    if (FAILED(NCryptFinalizeKey(hKey, 0))) {
        NCryptFreeObject(hKey);
        NCryptFreeObject(hProv);
        return result;
    }

    // 2. Encode subject name.
    BYTE name_buf[256];
    DWORD name_size = sizeof(name_buf);
    if (!CertStrToNameW(X509_ASN_ENCODING, L"CN=localhost",
                        CERT_OID_NAME_STR, nullptr, name_buf, &name_size,
                        nullptr)) {
        NCryptFreeObject(hKey);
        NCryptFreeObject(hProv);
        return result;
    }
    CERT_NAME_BLOB subject = {name_size, name_buf};

    // 3. Key provider info (CNG).
    CRYPT_KEY_PROV_INFO key_prov_info = {};
    key_prov_info.pwszContainerName = const_cast<wchar_t*>(kTestKeyName);
    key_prov_info.pwszProvName =
        const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
    key_prov_info.dwProvType = 0;
    key_prov_info.dwKeySpec = CERT_NCRYPT_KEY_SPEC;

    SYSTEMTIME expiry = {};
    GetSystemTime(&expiry);
    expiry.wYear += 1;

    // 4. Create self-signed cert with SHA-256 + Server Auth EKU (required for
    //    QUIC / TLS 1.3 via Schannel).
    CRYPT_ALGORITHM_IDENTIFIER sig_algo = {};
    sig_algo.pszObjId = const_cast<char*>(szOID_ECDSA_SHA256);

    // Add Server Authentication EKU.
    char* eku_oids[] = {const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_ENHKEY_USAGE eku = {};
    eku.cUsageIdentifier = 1;
    eku.rgpszUsageIdentifier = eku_oids;

    BYTE eku_buf[256];
    DWORD eku_size = sizeof(eku_buf);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE,
                              &eku, 0, nullptr, eku_buf, &eku_size)) {
        NCryptFreeObject(hKey);
        NCryptFreeObject(hProv);
        return result;
    }

    CERT_EXTENSION ext = {};
    ext.pszObjId = const_cast<char*>(szOID_ENHANCED_KEY_USAGE);
    ext.fCritical = FALSE;
    ext.Value.cbData = eku_size;
    ext.Value.pbData = eku_buf;

    CERT_EXTENSIONS exts = {};
    exts.cExtension = 1;
    exts.rgExtension = &ext;

    PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(
        static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(hKey),
        &subject, 0, &key_prov_info, &sig_algo, nullptr, &expiry, &exts);

    NCryptFreeObject(hKey);
    NCryptFreeObject(hProv);

    if (!cert) return result;

    // 5. Add to CurrentUser\MY so Schannel can find the private key.
    HCERTSTORE store =
        CertOpenSystemStoreW(0, L"MY");
    if (!store) {
        CertFreeCertificateContext(cert);
        return result;
    }

    PCCERT_CONTEXT store_cert = nullptr;
    if (!CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_ALWAYS,
                                           &store_cert)) {
        CertFreeCertificateContext(cert);
        CertCloseStore(store, 0);
        return result;
    }
    CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);

    // 6. Get SHA1 thumbprint.
    BYTE sha1[20] = {};
    DWORD sha1_size = sizeof(sha1);
    if (!CertGetCertificateContextProperty(store_cert,
                                            CERT_HASH_PROP_ID, sha1,
                                            &sha1_size)) {
        CertFreeCertificateContext(store_cert);
        return result;
    }

    result.context = store_cert;
    memcpy(result.hash.ShaHash, sha1, 20);
    result.valid = true;
    return result;
}

/// Remove the test certificate from the store and delete the CNG key.
static void FreeSelfSignedTestCert(TestCert& tc) {
    if (tc.context) {
        // Delete from store.
        CertDeleteCertificateFromStore(tc.context);
        tc.context = nullptr;
    }

    // Delete the CNG key.
    NCRYPT_PROV_HANDLE hProv = 0;
    if (SUCCEEDED(NCryptOpenStorageProvider(&hProv, MS_KEY_STORAGE_PROVIDER,
                                            0))) {
        NCRYPT_KEY_HANDLE hKey = 0;
        if (SUCCEEDED(NCryptOpenKey(hProv, &hKey, kTestKeyName, 0, 0))) {
            NCryptDeleteKey(hKey, 0);
        }
        NCryptFreeObject(hProv);
    }
    tc.valid = false;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

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

struct QuicTestFixture {
    QuicTransportServer server;
    QuicTransportClient client;
    std::shared_ptr<TransportConnection> server_conn;
    std::shared_ptr<TransportConnection> client_conn;
    TestCert cert;

    bool Setup() {
        cert = CreateSelfSignedTestCert();
        if (!cert.valid) {
            printf("  [Failed to create self-signed cert (GetLastError=%lu)]\n",
                   GetLastError());
            return false;
        }
        // cert created and added to store successfully.

        TransportConfig server_config;
        server_config.address = "127.0.0.1";
        server_config.port = 0;  // Ephemeral port

        auto init_result = server.Initialize(server_config);
        if (!init_result.ok()) {
            printf("  [Server init failed: %s]\n",
                   init_result.error().message.c_str());
            return false;
        }

        server.SetCertificateHash(cert.hash);

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
        if (!start_result.ok()) {
            // SEC_E_ALGORITHM_MISMATCH (0x80090331) means Schannel on this
            // Windows build doesn't support QUIC TLS 1.3 cipher suites.
            if (start_result.error().message.find("80090331") !=
                std::string::npos) {
                throw test_skipped();
            }
            printf("  [Server start failed: %s]\n",
                   start_result.error().message.c_str());
            return false;
        }

        TransportConfig client_config;
        client_config.address = "127.0.0.1";
        client_config.port = server.GetListenPort();

        auto client_init = client.Initialize(client_config);
        if (!client_init.ok()) {
            printf("  [Client init failed: %s]\n",
                   client_init.error().message.c_str());
            return false;
        }

        bool client_got_conn = false;
        auto connect_result = client.Connect(
            [&](std::shared_ptr<TransportConnection> conn) {
                std::lock_guard<std::mutex> lock(mtx);
                client_conn = conn;
                client_got_conn = true;
                cv.notify_all();
            });
        if (!connect_result.ok()) {
            printf("  [Client connect failed: %s]\n",
                   connect_result.error().message.c_str());
            return false;
        }

        return WaitFor([&] {
            std::lock_guard<std::mutex> lock(mtx);
            return server_got_conn && client_got_conn;
        }, 10000);
    }

    void Teardown() {
        if (client_conn) client_conn->Close();
        if (server_conn) server_conn->Close();
        // Give msquic time to process shutdown events.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        client.Shutdown();
        server.Shutdown();
        if (cert.valid) {
            FreeSelfSignedTestCert(cert);
        }
    }
};

static void RunTest(const char* name, std::function<void()> fn) {
    printf("  %-50s ", name);
    try {
        fn();
        printf("PASS\n");
        tests_passed++;
    } catch (const test_skipped&) {
        printf("SKIP (Schannel QUIC TLS 1.3 not available)\n");
        tests_skipped++;
    } catch (const std::exception& e) {
        printf("FAIL (%s)\n", e.what());
        tests_failed++;
    } catch (...) {
        printf("FAIL\n");
        tests_failed++;
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/// Test: small video frame delivered over QUIC stream.
static void test_small_frame_delivery() {
    QuicTestFixture f;
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

    // Give the server time to set up stream callbacks.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::vector<uint8_t> payload(100, 0xAA);
    SendOptions opts;
    opts.frame_index = 1;
    opts.timestamp_us = 12345;
    auto result = f.client_conn->GetChannel(ChannelType::kVideo)->Send(
        payload.data(), payload.size(), opts);
    ASSERT_TRUE(result.ok());

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

/// Test: large video frame (500KB) delivered over QUIC stream.
static void test_large_frame_delivery() {
    QuicTestFixture f;
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

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const size_t frame_size = 500 * 1024;
    std::vector<uint8_t> payload(frame_size);
    std::iota(payload.begin(), payload.end(), static_cast<uint8_t>(0));

    SendOptions opts;
    opts.frame_index = 2;
    opts.is_keyframe = true;
    auto result = f.client_conn->GetChannel(ChannelType::kVideo)->Send(
        payload.data(), payload.size(), opts);
    ASSERT_TRUE(result.ok());

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(mtx);
        return got_data;
    }, 15000));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_EQ(received.size(), payload.size());
        ASSERT_TRUE(received == payload);
    }

    f.Teardown();
}

/// Test: all three channels (video stream, audio datagram, control stream).
static void test_all_channels() {
    QuicTestFixture f;
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

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

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

    ASSERT_TRUE(WaitFor([&] { return channels_received.load() >= 3; }, 10000));

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_TRUE(video_data == v_payload);
        ASSERT_TRUE(audio_data == a_payload);
        ASSERT_TRUE(control_data == c_payload);
    }

    f.Teardown();
}

/// Test: disconnect detection when client closes.
static void test_disconnect_detection() {
    QuicTestFixture f;
    ASSERT_TRUE(f.Setup());

    std::atomic<bool> server_saw_disconnect{false};
    f.server_conn->SetStateCallback([&](ConnectionState state) {
        if (state == ConnectionState::kFailed ||
            state == ConnectionState::kDisconnected) {
            server_saw_disconnect = true;
        }
    });

    f.client_conn->Close();

    ASSERT_TRUE(WaitFor([&] { return server_saw_disconnect.load(); }, 10000));

    f.Teardown();
}

/// Test: multiple control messages all arrive (validates unique frame_index).
static void test_multiple_control_messages() {
    QuicTestFixture f;
    ASSERT_TRUE(f.Setup());

    std::mutex mtx;
    std::vector<std::vector<uint8_t>> received_messages;

    f.server_conn->SetControlMessageCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            received_messages.emplace_back(data, data + size);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

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

/// Test: audio delivery over QUIC datagrams (unreliable channel).
static void test_audio_datagram_delivery() {
    QuicTestFixture f;
    ASSERT_TRUE(f.Setup());

    std::mutex mtx;
    std::vector<uint8_t> received;
    bool got_data = false;

    f.server_conn->GetChannel(ChannelType::kAudio)->SetReceiveCallback(
        [&](const uint8_t* data, size_t size) {
            std::lock_guard<std::mutex> lock(mtx);
            received.assign(data, data + size);
            got_data = true;
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Typical Opus frame: ~320 bytes, fits in a single datagram.
    std::vector<uint8_t> payload(320, 0x55);
    SendOptions opts;
    opts.frame_index = 1;
    opts.timestamp_us = 20000;
    auto result = f.client_conn->GetChannel(ChannelType::kAudio)->Send(
        payload.data(), payload.size(), opts);
    ASSERT_TRUE(result.ok());

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

/// Test: video channel reports reliable delivery, audio reports unreliable.
static void test_channel_reliability_modes() {
    QuicTestFixture f;
    ASSERT_TRUE(f.Setup());

    auto* video = f.client_conn->GetChannel(ChannelType::kVideo);
    auto* audio = f.client_conn->GetChannel(ChannelType::kAudio);
    auto* control = f.client_conn->GetChannel(ChannelType::kControl);

    ASSERT_EQ(video->GetReliability(), ReliabilityMode::kReliableOrdered);
    ASSERT_EQ(audio->GetReliability(), ReliabilityMode::kUnreliable);
    ASSERT_EQ(control->GetReliability(), ReliabilityMode::kReliableOrdered);

    f.Teardown();
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    printf("=== QUIC Transport Integration Tests ===\n");

    RunTest("small_frame_delivery", test_small_frame_delivery);
    RunTest("large_frame_delivery", test_large_frame_delivery);
    RunTest("all_channels", test_all_channels);
    RunTest("disconnect_detection", test_disconnect_detection);
    RunTest("multiple_control_messages", test_multiple_control_messages);
    RunTest("audio_datagram_delivery", test_audio_datagram_delivery);
    RunTest("channel_reliability_modes", test_channel_reliability_modes);

    printf("\nResults: %d passed, %d failed, %d skipped\n",
           tests_passed, tests_failed, tests_skipped);
    if (tests_skipped > 0 && tests_failed == 0) {
        printf("Note: QUIC tests require Windows 11 or Windows Server 2022+\n"
               "      for full Schannel TLS 1.3 QUIC support.\n");
    }
    return tests_failed > 0 ? 1 : 0;
}
