#pragma once

#include "lumen/transport/transport_server.h"
#include "lumen/transport/transport_types.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace lumen {

class QuicheServer;
class WebTransportConnection;
struct WebTransportSessionInfo;

/// WebTransport-based transport server.
///
/// Wraps a QuicheServer (QUIC + HTTP/3 + WebTransport over BoringSSL) behind
/// the abstract TransportServer interface. Each established WebTransport
/// session is surfaced as a WebTransportConnection via the
/// IncomingConnectionCallback; inbound datagrams are routed to that
/// connection's channels.
///
/// This is the browser-facing receive path (the browser is the WT client).
/// Native→native QUIC client support is intentionally out of scope here.
class WebTransportServer : public TransportServer {
public:
    WebTransportServer();
    ~WebTransportServer() override;

    Result<void> Initialize(const TransportConfig& config) override;
    Result<void> Start(IncomingConnectionCallback callback) override;
    void Stop() override;
    void Shutdown() override;
    std::string GetListenAddress() const override;
    uint16_t GetListenPort() const override;

private:
    void OnSession(const WebTransportSessionInfo& info);
    void OnDatagram(const std::string& conn_id, uint64_t session_id,
                    const uint8_t* data, size_t size);

    static std::string MakeKey(const std::string& conn_id, uint64_t session_id);

    std::unique_ptr<QuicheServer> server_;
    TransportConfig config_;
    IncomingConnectionCallback incoming_callback_;

    mutable std::mutex mu_;
    // Keyed by conn_id + session_id. Holds the only owning reference besides
    // the one handed to the user via IncomingConnectionCallback.
    std::unordered_map<std::string, std::shared_ptr<WebTransportConnection>>
        conns_;
};

}  // namespace lumen
