#pragma once

#include "lumen/common/error.h"
#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <memory>

namespace lumen {

/// Policy controlling automatic reconnection attempts.
struct ReconnectPolicy {
    bool auto_reconnect = false;
    uint32_t max_attempts = 5;
    uint32_t initial_backoff_ms = 500;
    uint32_t max_backoff_ms = 30000;
};

/// Callback fired when a client connection is established.
using ClientConnectedCallback =
    std::function<void(std::shared_ptr<TransportConnection> connection)>;

/// Abstract interface for a transport client.
///
/// Manages outbound connections to a remote transport server.
class TransportClient {
public:
    virtual ~TransportClient() = default;

    /// Initialize the client with the given configuration.
    virtual Result<void> Initialize(const TransportConfig& config) = 0;

    /// Begin connecting to the remote server.
    /// The callback fires once the connection is established (or fails).
    virtual Result<void> Connect(ClientConnectedCallback callback) = 0;

    /// Set the reconnection policy for dropped connections.
    virtual void SetReconnectPolicy(const ReconnectPolicy& policy) = 0;

    /// Shut down the client, closing all connections.
    virtual void Shutdown() = 0;
};

}  // namespace lumen
