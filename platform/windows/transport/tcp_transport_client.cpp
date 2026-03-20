#include "tcp_transport_client.h"
#include "tcp_transport_connection.h"

#include <chrono>
#include <thread>

namespace lumen {

TcpTransportClient::TcpTransportClient() = default;

TcpTransportClient::~TcpTransportClient() {
    Shutdown();
}

Result<void> TcpTransportClient::Initialize(const TransportConfig& config) {
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

Result<void> TcpTransportClient::Connect(ClientConnectedCallback callback) {
    if (!initialized_) {
        return Error::Make(ErrorCode::kNotInitialized);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    shutdown_requested_ = false;
    connect_thread_ = std::thread(&TcpTransportClient::ConnectThreadFunc, this,
                                   std::move(callback));
    return {};
}

void TcpTransportClient::SetReconnectPolicy(const ReconnectPolicy& policy) {
    reconnect_policy_ = policy;
}

void TcpTransportClient::Shutdown() {
    shutdown_requested_ = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_connection_) {
            active_connection_->Close();
            active_connection_.reset();
        }
    }

    if (connect_thread_.joinable()) {
        connect_thread_.join();
    }

    if (initialized_) {
        WSACleanup();
        initialized_ = false;
    }
}

Result<SOCKET> TcpTransportClient::CreateAndConnect() {
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(config_.port);
    int ret = getaddrinfo(config_.address.c_str(), port_str.c_str(),
                          &hints, &result);
    if (ret != 0) {
        return Error::Make(ErrorCode::kTransportAddressInvalid,
                          "getaddrinfo failed: " + std::to_string(ret));
    }

    SOCKET sock = INVALID_SOCKET;
    for (auto* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = ::socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        // Set connect timeout via non-blocking + select.
        u_long non_blocking = 1;
        ioctlsocket(sock, FIONBIO, &non_blocking);

        int conn_result = ::connect(sock, ptr->ai_addr,
                                     static_cast<int>(ptr->ai_addrlen));
        if (conn_result == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(sock, &write_fds);

                timeval tv;
                tv.tv_sec = config_.connect_timeout_ms / 1000;
                tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

                int sel = ::select(0, nullptr, &write_fds, nullptr, &tv);
                if (sel <= 0) {
                    ::closesocket(sock);
                    sock = INVALID_SOCKET;
                    continue;
                }

                // Check if connection actually succeeded.
                int opt_val = 0;
                int opt_len = sizeof(opt_val);
                getsockopt(sock, SOL_SOCKET, SO_ERROR,
                          reinterpret_cast<char*>(&opt_val), &opt_len);
                if (opt_val != 0) {
                    ::closesocket(sock);
                    sock = INVALID_SOCKET;
                    continue;
                }
            } else {
                ::closesocket(sock);
                sock = INVALID_SOCKET;
                continue;
            }
        }

        // Restore blocking mode.
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        break;
    }

    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        return Error::Make(ErrorCode::kTransportConnectionFailed,
                          "Failed to connect to " + config_.address + ":" +
                              std::to_string(config_.port));
    }

    // Disable Nagle's algorithm for low-latency sends.
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));

    return sock;
}

void TcpTransportClient::ConnectThreadFunc(ClientConnectedCallback callback) {
    uint32_t attempt = 0;
    uint32_t backoff_ms = reconnect_policy_.initial_backoff_ms;

    while (!shutdown_requested_) {
        auto result = CreateAndConnect();
        if (result.ok()) {
            std::string remote = config_.address + ":" +
                                 std::to_string(config_.port);
            auto conn = std::make_shared<TcpTransportConnection>(
                result.value(), remote, config_.max_payload_size);
            conn->Start();

            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_connection_ = conn;
            }

            if (callback) callback(conn);
            return;
        }

        attempt++;
        if (!reconnect_policy_.auto_reconnect ||
            attempt >= reconnect_policy_.max_attempts) {
            if (callback) callback(nullptr);
            return;
        }

        // Exponential backoff.
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
        backoff_ms = (std::min)(backoff_ms * 2, reconnect_policy_.max_backoff_ms);
    }
}

}  // namespace lumen
