#include "ws_handshake.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace lumen {

namespace {

constexpr const char* kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string ToLowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return s;
}

std::string Trim(std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' ||
                     s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

bool Sha1(const std::string& in, uint8_t out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS s = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM,
                                              nullptr, 0);
    if (s != 0) return false;

    BCRYPT_HASH_HANDLE hash = nullptr;
    s = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (s != 0) { BCryptCloseAlgorithmProvider(alg, 0); return false; }

    s = BCryptHashData(hash,
                       reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())),
                       static_cast<ULONG>(in.size()), 0);
    if (s == 0) {
        s = BCryptFinishHash(hash, out, 20, 0);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return s == 0;
}

std::string Base64(const uint8_t* data, size_t size) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t b = (static_cast<uint32_t>(data[i]) << 16);
        if (i + 1 < size) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) b |= static_cast<uint32_t>(data[i + 2]);
        out.push_back(kTable[(b >> 18) & 0x3F]);
        out.push_back(kTable[(b >> 12) & 0x3F]);
        out.push_back(i + 1 < size ? kTable[(b >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < size ? kTable[b & 0x3F] : '=');
    }
    return out;
}

}  // namespace

std::string ComputeAcceptKey(const std::string& key) {
    uint8_t hash[20];
    if (!Sha1(key + kGuid, hash)) return {};
    return Base64(hash, sizeof(hash));
}

WsHandshakeStatus ParseUpgradeRequest(const char* data, size_t size,
                                       WsUpgradeRequest& out,
                                       size_t* consumed) {
    // Locate end of headers (CRLFCRLF).
    const char* end = nullptr;
    for (size_t i = 3; i < size; ++i) {
        if (data[i - 3] == '\r' && data[i - 2] == '\n' &&
            data[i - 1] == '\r' && data[i] == '\n') {
            end = data + i + 1;
            break;
        }
    }
    if (!end) return WsHandshakeStatus::kNeedMoreData;

    if (consumed) *consumed = static_cast<size_t>(end - data);

    // Split the request into lines.
    std::string headers(data, end - data - 2);  // strip trailing CRLF
    std::istringstream iss(headers);
    std::string line;

    if (!std::getline(iss, line)) return WsHandshakeStatus::kBadRequest;
    {
        std::istringstream rl(line);
        std::string version;
        rl >> out.method >> out.path >> version;
        if (out.method.empty() || out.path.empty() || version.rfind("HTTP/", 0) != 0) {
            return WsHandshakeStatus::kBadRequest;
        }
    }

    bool has_upgrade_websocket = false;
    bool has_connection_upgrade = false;
    std::string sec_version;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) return WsHandshakeStatus::kBadRequest;
        std::string name  = ToLowerAscii(Trim(line.substr(0, colon)));
        std::string value = Trim(line.substr(colon + 1));

        if (name == "upgrade") {
            if (ToLowerAscii(value).find("websocket") != std::string::npos) {
                has_upgrade_websocket = true;
            }
        } else if (name == "connection") {
            if (ToLowerAscii(value).find("upgrade") != std::string::npos) {
                has_connection_upgrade = true;
            }
        } else if (name == "sec-websocket-key") {
            out.sec_websocket_key = value;
        } else if (name == "sec-websocket-version") {
            sec_version = value;
        } else if (name == "sec-websocket-protocol") {
            out.sec_websocket_protocol = value;
        }
    }

    if (!has_upgrade_websocket || !has_connection_upgrade) {
        return WsHandshakeStatus::kNotUpgrade;
    }
    if (sec_version != "13") return WsHandshakeStatus::kNotUpgrade;
    if (out.sec_websocket_key.empty()) return WsHandshakeStatus::kMissingKey;

    return WsHandshakeStatus::kOk;
}

std::string BuildUpgradeResponse(const std::string& sec_websocket_key,
                                  const std::string& sec_websocket_protocol) {
    std::ostringstream o;
    o << "HTTP/1.1 101 Switching Protocols\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Accept: " << ComputeAcceptKey(sec_websocket_key) << "\r\n";
    if (!sec_websocket_protocol.empty()) {
        o << "Sec-WebSocket-Protocol: " << sec_websocket_protocol << "\r\n";
    }
    o << "\r\n";
    return o.str();
}

}  // namespace lumen
