#pragma once

// Self-signed ECDSA P-256 certificate helpers for Windows.
//
// Used by:
//   - QUIC transport tests (existing)
//   - HTTP/3 / WebTransport server (new — needs the SHA-256 of the DER cert
//     so the browser can pin via WebTransport's serverCertificateHashes)
//
// The cert is created in CurrentUser\MY so Schannel can find the private
// key when msquic loads the credential.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <msquic.h>

#include <array>
#include <cstdint>
#include <string>

namespace lumen {

/// A self-signed ECDSA P-256 cert, owned by the caller.
struct SelfSignedCert {
    /// Cert context. Owned — call ReleaseSelfSignedCert to clean up.
    PCCERT_CONTEXT context = nullptr;

    /// SHA-1 thumbprint, useful for QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH /
    /// _STORE so msquic can find the key in CurrentUser\MY.
    QUIC_CERTIFICATE_HASH sha1_thumbprint = {};

    /// SHA-256 of the full DER-encoded cert. This is the value the browser
    /// expects in WebTransport's `serverCertificateHashes` option.
    std::array<uint8_t, 32> sha256_der = {};

    /// CNG key container name used (so the caller can clean it up later).
    std::wstring key_name;

    /// True if everything succeeded.
    bool valid = false;
};

/// Create a self-signed ECDSA P-256 certificate, persisted in
/// CurrentUser\MY. `key_name` must be unique per process so concurrent
/// instances do not collide. `subject_cn` is the common name (e.g.
/// "lumen-receiver"). `validity_days` is capped per the WebTransport spec
/// at 14; pass <= 14 to be safe.
SelfSignedCert CreateSelfSignedCert(const std::wstring& key_name,
                                     const std::wstring& subject_cn,
                                     int validity_days);

/// Remove the cert from the store and delete the CNG key. Idempotent.
void ReleaseSelfSignedCert(SelfSignedCert& cert);

/// Lowercase hex string of the SHA-256 fingerprint, useful for printing
/// and for putting into JSON.
std::string Sha256ToHex(const std::array<uint8_t, 32>& hash);

/// Export the certificate as a PEM string (BEGIN/END CERTIFICATE).
std::string ExportCertPem(const SelfSignedCert& cert);

/// Export the private key as a PEM PKCS#8 string (BEGIN/END PRIVATE KEY).
/// Returns an empty string on failure.
std::string ExportPrivKeyPem(const SelfSignedCert& cert);

/// Convenience: write both PEMs to `cert_path` and `key_path`. Overwrites
/// any existing files. Returns true on success.
bool WriteCertAndKeyPemFiles(const SelfSignedCert& cert,
                              const std::string& cert_path,
                              const std::string& key_path);

}  // namespace lumen
