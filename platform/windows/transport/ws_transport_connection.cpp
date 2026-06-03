#include "ws_transport_connection.h"

#include "ws_frame_codec.h"
#include "lumen/transport/wire_format.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace lumen {

namespace {

constexpr size_t kRecvChunk = 64 * 1024;
constexpr size_t kMaxRecvBuffer = 32 * 1024 * 1024;  // hard cap

}  // namespace

WsTransportConnection::WsTransportConnection(SOCKET socket,
                                               std::string remote_address)
    : socket_(socket), remote_address_(std::move(remote_address)) {
    video_   = std::make_unique<WsTransportChannel>(ChannelType::kVideo,   this);
    audio_   = std::make_unique<WsTransportChannel>(ChannelType::kAudio,   this);
    control_ = std::make_unique<WsTransportChannel>(ChannelType::kControl, this);
}

WsTransportConnection::~WsTransportConnection() {
    Close();
}

Result<void> WsTransportConnection::Start() {
    if (running_.exchange(true)) {
        return Error::Make(ErrorCode::kAlreadyInitialized,
                           "WsTransportConnection already started");
    }
    TransitionState(ConnectionState::kConnected);
    recv_thread_ = std::thread(&WsTransportConnection::ReceiveLoop, this);
    return {};
}

void WsTransportConnection::Close() {
    // Idempotent. closed_ guards the work; running_ alone is unsafe
    // because the recv thread may have flipped it on its own (e.g.,
    // after seeing a kClose opcode), and we still need to join.
    if (closed_.exchange(true)) return;

    running_.store(false);
    TransitionState(ConnectionState::kDraining);

    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    if (recv_thread_.joinable()) recv_thread_.join();

    TransitionState(ConnectionState::kDisconnected);
}

Result<size_t> WsTransportConnection::SendBinaryFromChannel(
    ChannelType channel, const uint8_t* data, size_t size) {
    if (state_.load() != ConnectionState::kConnected) {
        return Error::Make(ErrorCode::kTransportNotConnected);
    }
    if (size > 16 * 1024 * 1024) {
        return Error::Make(ErrorCode::kInvalidArgument, "payload too large");
    }

    // Single contiguous buffer: [channel_id:u8][payload]
    std::vector<uint8_t> body(size + 1);
    body[0] = wire::ChannelIdOf(channel);
    if (size > 0) std::memcpy(body.data() + 1, data, size);

    auto frame = WriteFrame(WsOpcode::kBinary, body.data(), body.size(),
                            /*mask=*/false, /*mask_key_be=*/0);

    std::lock_guard<std::mutex> lock(send_mu_);
    size_t sent = 0;
    while (sent < frame.size()) {
        int n = ::send(socket_,
                       reinterpret_cast<const char*>(frame.data() + sent),
                       static_cast<int>(frame.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            running_.store(false);
            return Error::Make(ErrorCode::kTransportError,
                               "send failed: " +
                                   std::to_string(WSAGetLastError()));
        }
        sent += static_cast<size_t>(n);
    }
    return size;
}

TransportChannel* WsTransportConnection::GetChannel(ChannelType type) {
    switch (type) {
        case ChannelType::kVideo:   return video_.get();
        case ChannelType::kAudio:   return audio_.get();
        case ChannelType::kControl: return control_.get();
    }
    return nullptr;
}

void WsTransportConnection::SetStateCallback(ConnectionStateCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    state_callback_ = std::move(cb);
}

Result<void> WsTransportConnection::RequestKeyframe() {
    // Phase 1: control channel carries arbitrary bytes; the application
    // owns the request format. Stub returns OK.
    return {};
}

Result<void> WsTransportConnection::SendControlMessage(const uint8_t* data,
                                                        size_t size) {
    auto r = SendBinaryFromChannel(ChannelType::kControl, data, size);
    if (!r) return r.error();
    return {};
}

void WsTransportConnection::SetControlMessageCallback(
    ControlMessageCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mu_);
    control_callback_ = std::move(cb);
}

void WsTransportConnection::TransitionState(ConnectionState s) {
    state_.store(s);
    ConnectionStateCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mu_);
        cb = state_callback_;
    }
    if (cb) cb(s);
}

void WsTransportConnection::DispatchBinary(const uint8_t* data, size_t size) {
    if (size == 0) return;
    ChannelType channel_id;
    if (!wire::ChannelTypeOf(data[0], channel_id)) return;
    const uint8_t* payload = data + 1;
    const size_t payload_size = size - 1;

    if (channel_id == ChannelType::kControl) {
        ControlMessageCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mu_);
            cb = control_callback_;
        }
        if (cb) cb(payload, payload_size);
        // Also fan out to the control channel callback for symmetry with
        // GetChannel(kControl)->SetReceiveCallback users.
    }
    auto* ch = GetChannel(channel_id);
    if (auto* wsc = dynamic_cast<WsTransportChannel*>(ch)) {
        wsc->OnDataReceived(payload, payload_size);
    }
}

void WsTransportConnection::ReceiveLoop() {
    std::vector<uint8_t> buf;
    buf.reserve(kRecvChunk);

    while (running_.load()) {
        const size_t old_size = buf.size();
        buf.resize(old_size + kRecvChunk);
        int n = ::recv(socket_,
                       reinterpret_cast<char*>(buf.data() + old_size),
                       kRecvChunk, 0);
        if (n == 0) {
            buf.resize(old_size);
            break;  // peer closed
        }
        if (n == SOCKET_ERROR) {
            buf.resize(old_size);
            break;
        }
        buf.resize(old_size + static_cast<size_t>(n));

        // Drain as many complete frames as possible.
        size_t cursor = 0;
        for (;;) {
            WsFrame frame;
            auto status = ParseFrame(buf.data() + cursor,
                                     buf.size() - cursor, frame);
            if (status == WsParseStatus::kNeedMoreData) break;
            if (status != WsParseStatus::kOk) {
                running_.store(false);
                return;
            }
            cursor += frame.consumed_bytes;

            switch (frame.opcode) {
                case WsOpcode::kBinary:
                    DispatchBinary(frame.payload.data(), frame.payload.size());
                    break;
                case WsOpcode::kPing: {
                    auto pong = WriteFrame(WsOpcode::kPong,
                                           frame.payload.data(),
                                           frame.payload.size(), false);
                    std::lock_guard<std::mutex> lock(send_mu_);
                    ::send(socket_, reinterpret_cast<const char*>(pong.data()),
                           static_cast<int>(pong.size()), 0);
                    break;
                }
                case WsOpcode::kClose:
                    running_.store(false);
                    break;
                default:
                    break;
            }

            if (buf.size() > kMaxRecvBuffer) {
                running_.store(false);
                return;
            }
        }

        if (cursor > 0) {
            buf.erase(buf.begin(), buf.begin() + cursor);
        }
    }

    TransitionState(ConnectionState::kDisconnected);
}

}  // namespace lumen
