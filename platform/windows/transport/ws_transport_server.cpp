#include "ws_transport_server.h"

#include "ws_handshake.h"
#include "ws_transport_connection.h"

#include <ws2tcpip.h>

#include <cstring>
#include <vector>

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

std::string SocketPeerAddr(SOCKET s) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(s, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        return std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    return "?";
}

bool ReadHttpRequest(SOCKET s, std::string& out) {
    out.clear();
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos) {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, buf + n);
        if (out.size() > 64 * 1024) return false;
    }
    return true;
}

bool WriteAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(s, data.data() + sent,
                       static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

WsTransportServer::WsTransportServer() = default;
WsTransportServer::~WsTransportServer() { Shutdown(); }

Result<void> WsTransportServer::Initialize(const TransportConfig& config) {
    if (!EnsureWsaStarted()) {
        return Error::Make(ErrorCode::kTransportError, "WSAStartup failed");
    }
    config_ = config;

    listen_socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET) {
        return Error::Make(ErrorCode::kTransportError, "socket() failed");
    }

    BOOL one = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config.port);
    if (::bind(listen_socket_, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) == SOCKET_ERROR) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return Error::Make(ErrorCode::kTransportError, "bind failed");
    }
    if (::listen(listen_socket_, 16) == SOCKET_ERROR) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return Error::Make(ErrorCode::kTransportError, "listen failed");
    }

    sockaddr_in actual{};
    int actual_len = sizeof(actual);
    if (getsockname(listen_socket_,
                    reinterpret_cast<sockaddr*>(&actual), &actual_len) == 0) {
        bound_port_ = ntohs(actual.sin_port);
    } else {
        bound_port_ = config.port;
    }
    return {};
}

Result<void> WsTransportServer::Start(IncomingConnectionCallback callback) {
    if (listen_socket_ == INVALID_SOCKET) {
        return Error::Make(ErrorCode::kNotInitialized);
    }
    if (running_.exchange(true)) {
        return Error::Make(ErrorCode::kAlreadyInitialized);
    }
    accept_thread_ = std::thread(&WsTransportServer::AcceptLoop, this,
                                  std::move(callback));
    return {};
}

void WsTransportServer::Stop() {
    if (!running_.exchange(false)) return;
    if (listen_socket_ != INVALID_SOCKET) {
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
    if (accept_thread_.joinable()) accept_thread_.join();
}

void WsTransportServer::Shutdown() {
    Stop();
    std::vector<std::shared_ptr<TransportConnection>> drained;
    {
        std::lock_guard<std::mutex> lock(mu_);
        drained.swap(connections_);
    }
    for (auto& c : drained) c->Close();
}

std::string WsTransportServer::GetListenAddress() const {
    return "0.0.0.0:" + std::to_string(bound_port_);
}

uint16_t WsTransportServer::GetListenPort() const { return bound_port_; }

void WsTransportServer::AcceptLoop(IncomingConnectionCallback callback) {
    while (running_.load()) {
        sockaddr_in addr{};
        int addr_len = sizeof(addr);
        SOCKET client = ::accept(listen_socket_,
                                  reinterpret_cast<sockaddr*>(&addr),
                                  &addr_len);
        if (client == INVALID_SOCKET) {
            if (running_.load()) continue;  // spurious; retry
            return;
        }

        std::string request;
        if (!ReadHttpRequest(client, request)) {
            closesocket(client);
            continue;
        }

        WsUpgradeRequest upg;
        size_t consumed = 0;
        auto status = ParseUpgradeRequest(request.data(), request.size(),
                                           upg, &consumed);
        if (status != WsHandshakeStatus::kOk) {
            const char* body = "HTTP/1.1 400 Bad Request\r\n"
                               "Content-Length: 0\r\n\r\n";
            ::send(client, body, static_cast<int>(std::strlen(body)), 0);
            closesocket(client);
            continue;
        }

        auto response = BuildUpgradeResponse(upg.sec_websocket_key,
                                              upg.sec_websocket_protocol);
        if (!WriteAll(client, response)) {
            closesocket(client);
            continue;
        }

        // The browser may have already sent WS frames before our 101 was
        // received, so anything past `consumed` is the start of frame
        // bytes. For Phase 1 we ignore that pre-buffered tail; the first
        // received frame would just have an immediate kNeedMoreData and
        // be picked up by the receive loop. (Web sender always opens the
        // socket and waits for `onopen` before sending, so this is a
        // theoretical concern, not a practical one.)
        (void)consumed;

        auto conn = std::make_shared<WsTransportConnection>(
            client, SocketPeerAddr(client));
        if (!conn->Start().ok()) {
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            connections_.push_back(conn);
        }
        if (callback) callback(conn);
    }
}

}  // namespace lumen
