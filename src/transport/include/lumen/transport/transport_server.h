#pragma once

#include "lumen/common/error.h"
#include "lumen/transport/transport_connection.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <memory>
#include <string>

namespace lumen {

/// Callback fired when a new inbound connection is accepted.
using IncomingConnectionCallback =
    std::function<void(std::shared_ptr<TransportConnection> connection)>;

/// Abstract interface for a transport server.
///
/// Listens for inbound connections and fires a callback for each.
class TransportServer {
public:
    virtual ~TransportServer() = default;

    /// Initialize the server with the given configuration.
    virtual Result<void> Initialize(const TransportConfig& config) = 0;

    /// Start listening and accepting connections.
    virtual Result<void> Start(IncomingConnectionCallback callback) = 0;

    /// Stop accepting new connections (existing connections remain open).
    virtual void Stop() = 0;

    /// Shut down the server, closing all connections.
    virtual void Shutdown() = 0;

    /// The address the server is bound to.
    virtual std::string GetListenAddress() const = 0;

    /// The port the server is listening on (useful when bound to port 0).
    virtual uint16_t GetListenPort() const = 0;
};

}  // namespace lumen
