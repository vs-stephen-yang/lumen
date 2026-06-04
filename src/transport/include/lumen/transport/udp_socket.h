#pragma once

// Thin, platform-neutral UDP socket used by the QUIC transport. Exists so the
// quiche server/client carry no winsock-specific code and can be reused on
// POSIX targets (Android / macOS / iOS) by swapping only the implementation.
//
// The interface speaks BSD sockaddr (available on every target); the concrete
// implementation owns the platform fd and the readiness wait (select/poll).

#include "lumen/common/error.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lumen {

class UdpSocket {
public:
    virtual ~UdpSocket() = default;

    /// Create the underlying UDP socket (and initialise winsock on Windows).
    virtual Result<void> Open() = 0;

    /// Bind to a local address (server side).
    virtual Result<void> Bind(const sockaddr* addr, socklen_t len) = 0;

    /// Set the default peer and let the OS resolve a concrete local address
    /// (client side). Recv/SendTo still take/return addresses explicitly.
    virtual Result<void> Connect(const sockaddr* addr, socklen_t len) = 0;

    /// Fill `out` with the bound local address (getsockname).
    virtual Result<void> GetLocalAddr(sockaddr_storage& out,
                                      socklen_t& len) const = 0;

    /// Block up to `timeout_ms` for readability. Returns true if the socket is
    /// readable, false on timeout, error, or close.
    virtual bool WaitReadable(int timeout_ms) = 0;

    /// Receive one datagram. Returns bytes read (>=0), or -1 on error.
    virtual int64_t Recv(uint8_t* buf, size_t len, sockaddr_storage& from,
                         socklen_t& from_len) = 0;

    /// Send one datagram to `to`. Returns bytes sent (>=0), or -1 on error.
    virtual int64_t SendTo(const uint8_t* buf, size_t len, const sockaddr* to,
                           socklen_t to_len) = 0;

    virtual void Close() = 0;
    virtual bool IsOpen() const = 0;
};

/// Construct the platform UDP socket implementation (winsock on Windows, BSD
/// sockets on POSIX). Defined in the per-platform translation unit.
std::unique_ptr<UdpSocket> MakeUdpSocket();

}  // namespace lumen
