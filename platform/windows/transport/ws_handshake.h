#pragma once

// HTTP/1.1 → WebSocket upgrade handshake (RFC 6455 §4).
//
// These helpers do *not* perform I/O. The server reads the HTTP request
// bytes from the socket, calls ParseUpgradeRequest, then writes the
// response from BuildUpgradeResponse back over the socket.

#include <cstdint>
#include <string>

namespace lumen {

enum class WsHandshakeStatus {
    kOk,
    kNeedMoreData,    // Request not yet terminated by CRLFCRLF.
    kBadRequest,      // Malformed HTTP.
    kNotUpgrade,      // Missing or wrong Upgrade/Connection/Version headers.
    kMissingKey,      // Sec-WebSocket-Key absent.
};

struct WsUpgradeRequest {
    std::string method;
    std::string path;
    std::string sec_websocket_key;     // Raw value from the header.
    std::string sec_websocket_protocol; // Optional.
};

/// Parse an HTTP request from the head of `data`. On kOk, `consumed`
/// tells the caller how many bytes belonged to the request (including
/// the terminating CRLFCRLF) so the rest can be treated as WS frames.
WsHandshakeStatus ParseUpgradeRequest(const char* data, size_t size,
                                       WsUpgradeRequest& out,
                                       size_t* consumed);

/// Build the HTTP/1.1 101 Switching Protocols response, including the
/// computed Sec-WebSocket-Accept value.
std::string BuildUpgradeResponse(const std::string& sec_websocket_key,
                                  const std::string& sec_websocket_protocol);

/// Visible for tests: SHA-1 + base64 of `key + magic GUID` per RFC 6455.
std::string ComputeAcceptKey(const std::string& sec_websocket_key);

}  // namespace lumen
