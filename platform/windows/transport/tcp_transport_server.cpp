#include "tcp_transport_server.h"
#include "tcp_transport_connection.h"

#include <algorithm>

namespace lumen {

TcpTransportServer::TcpTransportServer() = default;

TcpTransportServer::~TcpTransportServer() {
    Shutdown();
}

Result<void> TcpTransportServer::Initialize(const TransportConfig& config) {
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        return Error::Make(ErrorCode::kTransportError,
                          "WSAStartup failed: " + std::to_string(result));
    }

    config_ = config;
    initialized_ = true;
    return {};
}

Result<void> TcpTransportServer::Start(IncomingConnectionCallback callback) {
    if (!initialized_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    std::string port_str = std::to_string(config_.port);
    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(
        config_.address.empty() ? nullptr : config_.address.c_str(),
        port_str.c_str(), &hints, &result);
    if (ret != 0) {
        return Error::Make(ErrorCode::kTransportAddressInvalid,
                          "getaddrinfo failed: " + std::to_string(ret));
    }

    listen_socket_ = ::socket(result->ai_family, result->ai_socktype,
                               result->ai_protocol);
    if (listen_socket_ == INVALID_SOCKET) {
        freeaddrinfo(result);
        return Error::Make(ErrorCode::kTransportError, "socket() failed");
    }

    // Allow port reuse.
    int opt = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    if (::bind(listen_socket_, result->ai_addr,
               static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR) {
        freeaddrinfo(result);
        ::closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return Error::Make(ErrorCode::kTransportError, "bind() failed");
    }
    freeaddrinfo(result);

    if (::listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR) {
        ::closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
        return Error::Make(ErrorCode::kTransportError, "listen() failed");
    }

    // Retrieve the actual bound port (important when config_.port == 0).
    struct sockaddr_in addr;
    int addr_len = sizeof(addr);
    if (getsockname(listen_socket_, reinterpret_cast<struct sockaddr*>(&addr),
                    &addr_len) == 0) {
        bound_port_ = ntohs(addr.sin_port);
    }

    stop_requested_ = false;
    accept_thread_ = std::thread(&TcpTransportServer::AcceptThreadFunc, this,
                                  std::move(callback));
    return {};
}

void TcpTransportServer::Stop() {
    stop_requested_ = true;

    // Close the listen socket to unblock accept().
    if (listen_socket_ != INVALID_SOCKET) {
        ::closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void TcpTransportServer::Shutdown() {
    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& conn : connections_) {
            conn->Close();
        }
        connections_.clear();
    }

    if (initialized_) {
        WSACleanup();
        initialized_ = false;
    }
}

std::string TcpTransportServer::GetListenAddress() const {
    return config_.address.empty() ? "0.0.0.0" : config_.address;
}

uint16_t TcpTransportServer::GetListenPort() const {
    return bound_port_;
}

void TcpTransportServer::AcceptThreadFunc(
    IncomingConnectionCallback callback) {
    while (!stop_requested_) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listen_socket_, &read_fds);

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 50000;  // 50ms

        int sel = ::select(0, &read_fds, nullptr, nullptr, &timeout);
        if (sel == SOCKET_ERROR || stop_requested_) break;
        if (sel == 0) continue;

        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_socket =
            ::accept(listen_socket_,
                     reinterpret_cast<struct sockaddr*>(&client_addr),
                     &client_len);
        if (client_socket == INVALID_SOCKET) continue;

        // Disable Nagle's algorithm.
        int flag = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&flag), sizeof(flag));

        // Format remote address.
        char addr_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, addr_buf, sizeof(addr_buf));
        std::string remote = std::string(addr_buf) + ":" +
                             std::to_string(ntohs(client_addr.sin_port));

        auto conn = std::make_shared<TcpTransportConnection>(
            client_socket, remote, config_.max_payload_size);
        conn->Start();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Sweep dead connections before adding new one.
            connections_.erase(
                std::remove_if(connections_.begin(), connections_.end(),
                    [](const auto& c) {
                        auto s = c->GetState();
                        return s == ConnectionState::kDisconnected ||
                               s == ConnectionState::kFailed;
                    }),
                connections_.end());
            connections_.push_back(conn);
        }

        if (callback) callback(conn);
    }
}

}  // namespace lumen
