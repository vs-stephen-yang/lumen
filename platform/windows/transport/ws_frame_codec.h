#pragma once

// RFC 6455 WebSocket frame parser/serializer.
//
// Stateless byte-level codec — no I/O, no allocation policy. The
// connection layer owns the receive buffer and feeds bytes to ParseFrame
// as they arrive.
//
// We support: binary, text, close, ping, pong. We do not support
// fragmentation (FIN=0). The web sender always sends complete messages.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lumen {

enum class WsOpcode : uint8_t {
    kContinuation = 0x0,
    kText         = 0x1,
    kBinary       = 0x2,
    kClose        = 0x8,
    kPing         = 0x9,
    kPong         = 0xA,
};

enum class WsParseStatus {
    kOk,            // A frame was parsed; payload is valid.
    kNeedMoreData,  // Buffer is short; call again when more bytes arrive.
    kProtocolError, // Malformed; caller should drop the connection.
    kFragmented,    // FIN=0 — we don't support fragmented frames.
    kReservedBits,  // RSV1/2/3 set without negotiated extension.
};

struct WsFrame {
    WsOpcode opcode = WsOpcode::kBinary;
    bool fin = true;
    std::vector<uint8_t> payload;     // Unmasked.
    size_t consumed_bytes = 0;        // How many bytes from `data` were used.
};

/// Parse a single WebSocket frame from the head of `data`.
/// On kOk, `out` contains the parsed frame and `out.consumed_bytes` tells
/// the caller how many bytes to drop from its receive buffer.
WsParseStatus ParseFrame(const uint8_t* data, size_t size, WsFrame& out);

/// Serialize a frame.
/// `mask` controls masking — clients MUST mask, servers MUST NOT.
/// `mask_key_be` is a 32-bit big-endian masking key (caller-provided so
/// tests can be deterministic).
std::vector<uint8_t> WriteFrame(WsOpcode opcode,
                                 const uint8_t* payload, size_t payload_size,
                                 bool mask, uint32_t mask_key_be = 0);

}  // namespace lumen
