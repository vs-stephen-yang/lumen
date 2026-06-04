#include "lumen/transport/udp_socket.h"

#include <poll.h>
#include <unistd.h>

#include <cstring>

// POSIX UDP socket implementation (Android / macOS / iOS / Linux). Built only
// for non-Windows targets — see platform/posix/CMakeLists.txt. Mirrors
// udp_socket_win.cpp so the quiche server/client are byte-for-byte the same
// across platforms; only the socket primitive differs.

namespace lumen {

namespace {

class UdpSocketPosix : public UdpSocket {
public:
    ~UdpSocketPosix() override { Close(); }

    Result<void> Open() override {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            return Error::Make(ErrorCode::kTransportError, "socket() failed");
        }
        return {};
    }

    Result<void> Bind(const sockaddr* addr, socklen_t len) override {
        if (::bind(fd_, addr, len) != 0) {
            return Error::Make(ErrorCode::kTransportError, "bind() failed");
        }
        return {};
    }

    Result<void> Connect(const sockaddr* addr, socklen_t len) override {
        if (::connect(fd_, addr, len) != 0) {
            return Error::Make(ErrorCode::kTransportConnectionFailed,
                               "connect() failed");
        }
        return {};
    }

    Result<void> GetLocalAddr(sockaddr_storage& out,
                              socklen_t& len) const override {
        socklen_t l = sizeof(out);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&out), &l) != 0) {
            return Error::Make(ErrorCode::kTransportError, "getsockname failed");
        }
        len = l;
        return {};
    }

    bool WaitReadable(int timeout_ms) override {
        if (fd_ < 0) return false;
        pollfd p{};
        p.fd = fd_;
        p.events = POLLIN;
        const int r = ::poll(&p, 1, timeout_ms);
        return r > 0 && (p.revents & POLLIN) != 0;
    }

    int64_t Recv(uint8_t* buf, size_t len, sockaddr_storage& from,
                 socklen_t& from_len) override {
        from_len = sizeof(from);
        const ssize_t n = ::recvfrom(fd_, buf, len, 0,
                                     reinterpret_cast<sockaddr*>(&from),
                                     &from_len);
        return n < 0 ? -1 : static_cast<int64_t>(n);
    }

    int64_t SendTo(const uint8_t* buf, size_t len, const sockaddr* to,
                   socklen_t to_len) override {
        const ssize_t n = ::sendto(fd_, buf, len, 0, to, to_len);
        return n < 0 ? -1 : static_cast<int64_t>(n);
    }

    void Close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool IsOpen() const override { return fd_ >= 0; }

private:
    int fd_ = -1;
};

}  // namespace

std::unique_ptr<UdpSocket> MakeUdpSocket() {
    return std::make_unique<UdpSocketPosix>();
}

}  // namespace lumen
