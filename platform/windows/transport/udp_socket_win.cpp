#include "lumen/transport/udp_socket.h"

#include <mutex>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace lumen {

namespace {

bool EnsureWsaStarted() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        WSADATA wsa;
        ok = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    });
    return ok;
}

class UdpSocketWin : public UdpSocket {
public:
    ~UdpSocketWin() override { Close(); }

    Result<void> Open() override {
        if (!EnsureWsaStarted()) {
            return Error::Make(ErrorCode::kTransportError, "WSAStartup failed");
        }
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) {
            return Error::Make(ErrorCode::kTransportError, "socket() failed");
        }
        return {};
    }

    Result<void> Bind(const sockaddr* addr, socklen_t len) override {
        if (::bind(sock_, addr, len) == SOCKET_ERROR) {
            return Error::Make(ErrorCode::kTransportError,
                               "bind() failed: " +
                                   std::to_string(WSAGetLastError()));
        }
        return {};
    }

    Result<void> Connect(const sockaddr* addr, socklen_t len) override {
        if (::connect(sock_, addr, len) == SOCKET_ERROR) {
            return Error::Make(ErrorCode::kTransportConnectionFailed,
                               "connect() failed: " +
                                   std::to_string(WSAGetLastError()));
        }
        return {};
    }

    Result<void> GetLocalAddr(sockaddr_storage& out,
                              socklen_t& len) const override {
        int l = sizeof(out);
        if (getsockname(sock_, reinterpret_cast<sockaddr*>(&out), &l) != 0) {
            return Error::Make(ErrorCode::kTransportError, "getsockname failed");
        }
        len = static_cast<socklen_t>(l);
        return {};
    }

    bool WaitReadable(int timeout_ms) override {
        if (sock_ == INVALID_SOCKET) return false;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_, &fds);
        timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        return ::select(0, &fds, nullptr, nullptr, &tv) > 0 &&
               FD_ISSET(sock_, &fds);
    }

    int64_t Recv(uint8_t* buf, size_t len, sockaddr_storage& from,
                 socklen_t& from_len) override {
        int fl = sizeof(from);
        int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf),
                           static_cast<int>(len), 0,
                           reinterpret_cast<sockaddr*>(&from), &fl);
        if (n == SOCKET_ERROR) return -1;
        from_len = static_cast<socklen_t>(fl);
        return n;
    }

    int64_t SendTo(const uint8_t* buf, size_t len, const sockaddr* to,
                   socklen_t to_len) override {
        int n = ::sendto(sock_, reinterpret_cast<const char*>(buf),
                         static_cast<int>(len), 0, to, to_len);
        return n == SOCKET_ERROR ? -1 : n;
    }

    void Close() override {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool IsOpen() const override { return sock_ != INVALID_SOCKET; }

private:
    SOCKET sock_ = INVALID_SOCKET;
};

}  // namespace

std::unique_ptr<UdpSocket> MakeUdpSocket() {
    return std::make_unique<UdpSocketWin>();
}

}  // namespace lumen
