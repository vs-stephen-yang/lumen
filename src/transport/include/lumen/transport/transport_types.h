#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lumen {

/// Logical channel types multiplexed over a single connection.
enum class ChannelType {
    kVideo,
    kAudio,
    kControl,
};

/// Delivery guarantees for a transport channel.
enum class ReliabilityMode {
    kReliableOrdered,
    kReliableUnordered,
    kUnreliable,
    kPartiallyReliable,
};

/// Priority levels for send scheduling (lower value = higher priority).
enum class SendPriority : uint8_t {
    kControl = 0,
    kAudio = 1,
    kVideoKey = 2,
    kVideoDelta = 3,
    kBulk = 4,
};

/// Connection lifecycle states.
enum class ConnectionState {
    kDisconnected,
    kConnecting,
    kConnected,
    kDraining,
    kFailed,
};

/// TLS configuration for transport security.
struct TlsConfig {
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool verify_peer = true;
};

/// Configuration for establishing a transport connection.
struct TransportConfig {
    std::string address;
    uint16_t port = 0;
    TlsConfig tls;

    // Tuning
    uint32_t max_payload_size = 1200;           // MTU-safe payload size
    uint32_t send_buffer_size = 2 * 1024 * 1024;
    uint32_t recv_buffer_size = 2 * 1024 * 1024;
    uint32_t connect_timeout_ms = 5000;

    // NAT traversal
    std::string stun_server;
    std::string turn_server;
    std::string turn_username;
    std::string turn_password;
};

/// Runtime statistics from the transport layer.
struct TransportStats {
    uint64_t rtt_us = 0;
    uint64_t rtt_variance_us = 0;
    uint64_t bandwidth_estimate_bps = 0;
    double loss_rate = 0.0;              // 0.0 to 1.0
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t congestion_window = 0;
};

/// 28-byte serializable header prefixed to every media packet.
struct MediaPacketHeader {
    uint32_t ssrc = 0;               // Stream source identifier
    uint64_t frame_index = 0;        // Monotonically increasing frame number
    uint64_t timestamp_us = 0;       // Capture timestamp in microseconds
    uint16_t fragment_index = 0;     // Fragment index within this frame
    uint16_t fragment_count = 0;     // Total fragments in this frame
    uint32_t flags = 0;              // Bit 0: is_keyframe, Bit 1: is_last_fragment

    bool IsKeyframe() const { return (flags & 0x1) != 0; }
    bool IsLastFragment() const { return (flags & 0x2) != 0; }

    void SetKeyframe(bool v) {
        if (v) flags |= 0x1; else flags &= ~0x1u;
    }
    void SetLastFragment(bool v) {
        if (v) flags |= 0x2; else flags &= ~0x2u;
    }

    static constexpr size_t kSerializedSize = 28;

    /// Serialize to network byte order into a buffer of at least kSerializedSize bytes.
    void Serialize(uint8_t* buf) const;

    /// Deserialize from network byte order. Returns false on failure.
    static bool Deserialize(const uint8_t* buf, size_t len, MediaPacketHeader& out);
};

/// Callback aliases for transport events.
using ConnectionStateCallback = std::function<void(ConnectionState state)>;
using DataReceivedCallback = std::function<void(const uint8_t* data, size_t size)>;
using StatsCallback = std::function<void(const TransportStats& stats)>;

}  // namespace lumen
