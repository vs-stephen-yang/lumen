#include "cert_util.h"

#include <ncrypt.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "bcrypt.lib")

namespace lumen {

namespace {

bool Sha256(const uint8_t* data, size_t size, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                     nullptr, 0) != 0) {
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    bool ok = false;
    if (BCryptHashData(hash,
                        reinterpret_cast<PUCHAR>(const_cast<uint8_t*>(data)),
                        static_cast<ULONG>(size), 0) == 0 &&
        BCryptFinishHash(hash, out, 32, 0) == 0) {
        ok = true;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace

SelfSignedCert CreateSelfSignedCert(const std::wstring& key_name,
                                     const std::wstring& subject_cn,
                                     int validity_days) {
    SelfSignedCert result;
    result.key_name = key_name;

    NCRYPT_PROV_HANDLE prov = 0;
    if (FAILED(NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0))) {
        return result;
    }

    // Delete any stale key from a prior run with the same name.
    NCRYPT_KEY_HANDLE old = 0;
    if (SUCCEEDED(NCryptOpenKey(prov, &old, key_name.c_str(), 0, 0))) {
        NCryptDeleteKey(old, 0);
    }

    NCRYPT_KEY_HANDLE key = 0;
    if (FAILED(NCryptCreatePersistedKey(prov, &key,
                                         BCRYPT_ECDSA_P256_ALGORITHM,
                                         key_name.c_str(), 0,
                                         NCRYPT_OVERWRITE_KEY_FLAG))) {
        NCryptFreeObject(prov);
        return result;
    }

    // Mark the key exportable so ExportPrivKeyPem can dump it as PKCS#8
    // for quiche/BoringSSL. The default policy is non-exportable; this
    // MUST run before NCryptFinalizeKey.
    DWORD export_policy =
        NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY,
                       reinterpret_cast<PBYTE>(&export_policy),
                       sizeof(export_policy), 0);

    if (FAILED(NCryptFinalizeKey(key, 0))) {
        NCryptFreeObject(key);
        NCryptFreeObject(prov);
        return result;
    }

    // Subject name: CN=<subject_cn>
    std::wstring subject_str = L"CN=" + subject_cn;
    BYTE name_buf[256];
    DWORD name_size = sizeof(name_buf);
    if (!CertStrToNameW(X509_ASN_ENCODING, subject_str.c_str(),
                        CERT_OID_NAME_STR, nullptr, name_buf, &name_size,
                        nullptr)) {
        NCryptFreeObject(key);
        NCryptFreeObject(prov);
        return result;
    }
    CERT_NAME_BLOB subject = {name_size, name_buf};

    CRYPT_KEY_PROV_INFO key_prov_info = {};
    key_prov_info.pwszContainerName = const_cast<wchar_t*>(key_name.c_str());
    key_prov_info.pwszProvName =
        const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
    key_prov_info.dwProvType = 0;
    key_prov_info.dwKeySpec = CERT_NCRYPT_KEY_SPEC;

    SYSTEMTIME expiry = {};
    GetSystemTime(&expiry);
    if (validity_days < 1) validity_days = 1;
    // SYSTEMTIME doesn't add days directly; use FILETIME math.
    FILETIME ft = {};
    SystemTimeToFileTime(&expiry, &ft);
    ULARGE_INTEGER large;
    large.LowPart = ft.dwLowDateTime;
    large.HighPart = ft.dwHighDateTime;
    large.QuadPart += static_cast<ULONGLONG>(validity_days) *
                       24ULL * 60ULL * 60ULL * 10000000ULL;
    ft.dwLowDateTime = large.LowPart;
    ft.dwHighDateTime = large.HighPart;
    FileTimeToSystemTime(&ft, &expiry);

    CRYPT_ALGORITHM_IDENTIFIER sig_algo = {};
    sig_algo.pszObjId = const_cast<char*>(szOID_ECDSA_SHA256);

    char* eku_oids[] = {const_cast<char*>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_ENHKEY_USAGE eku = {};
    eku.cUsageIdentifier = 1;
    eku.rgpszUsageIdentifier = eku_oids;

    BYTE eku_buf[256];
    DWORD eku_size = sizeof(eku_buf);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE,
                              &eku, 0, nullptr, eku_buf, &eku_size)) {
        NCryptFreeObject(key);
        NCryptFreeObject(prov);
        return result;
    }
    CERT_EXTENSION ext = {};
    ext.pszObjId = const_cast<char*>(szOID_ENHANCED_KEY_USAGE);
    ext.fCritical = FALSE;
    ext.Value.cbData = eku_size;
    ext.Value.pbData = eku_buf;
    CERT_EXTENSIONS exts = {};
    exts.cExtension = 1;
    exts.rgExtension = &ext;

    PCCERT_CONTEXT cert = CertCreateSelfSignCertificate(
        static_cast<HCRYPTPROV_OR_NCRYPT_KEY_HANDLE>(key),
        &subject, 0, &key_prov_info, &sig_algo, nullptr, &expiry, &exts);

    NCryptFreeObject(key);
    NCryptFreeObject(prov);

    if (!cert) return result;

    HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
    if (!store) {
        CertFreeCertificateContext(cert);
        return result;
    }
    PCCERT_CONTEXT store_cert = nullptr;
    if (!CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_ALWAYS,
                                           &store_cert)) {
        CertFreeCertificateContext(cert);
        CertCloseStore(store, 0);
        return result;
    }
    CertFreeCertificateContext(cert);
    CertCloseStore(store, 0);

    BYTE sha1[20] = {};
    DWORD sha1_size = sizeof(sha1);
    if (!CertGetCertificateContextProperty(store_cert, CERT_HASH_PROP_ID,
                                            sha1, &sha1_size)) {
        CertFreeCertificateContext(store_cert);
        return result;
    }

    // SHA-256 of the DER-encoded cert (the value WebTransport pins on).
    uint8_t sha256[32] = {};
    if (!Sha256(store_cert->pbCertEncoded, store_cert->cbCertEncoded,
                sha256)) {
        CertFreeCertificateContext(store_cert);
        return result;
    }

    result.context = store_cert;
    std::memcpy(result.sha1_thumbprint.data(), sha1, 20);
    std::memcpy(result.sha256_der.data(), sha256, 32);
    result.valid = true;
    return result;
}

void ReleaseSelfSignedCert(SelfSignedCert& cert) {
    if (cert.context) {
        CertDeleteCertificateFromStore(cert.context);
        cert.context = nullptr;
    }
    if (!cert.key_name.empty()) {
        NCRYPT_PROV_HANDLE prov = 0;
        if (SUCCEEDED(NCryptOpenStorageProvider(&prov,
                                                 MS_KEY_STORAGE_PROVIDER,
                                                 0))) {
            NCRYPT_KEY_HANDLE key = 0;
            if (SUCCEEDED(NCryptOpenKey(prov, &key, cert.key_name.c_str(),
                                         0, 0))) {
                NCryptDeleteKey(key, 0);
            }
            NCryptFreeObject(prov);
        }
    }
    cert.valid = false;
}

std::string Sha256ToHex(const std::array<uint8_t, 32>& hash) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : hash) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

namespace {

std::string Base64WithLineBreaks(const uint8_t* data, size_t size) {
    DWORD pem_len = 0;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size),
                                CRYPT_STRING_BASE64,
                                nullptr, &pem_len)) {
        return {};
    }
    std::string out(pem_len, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(size),
                                CRYPT_STRING_BASE64,
                                out.data(), &pem_len)) {
        return {};
    }
    out.resize(pem_len);  // strip trailing NUL
    return out;
}

}  // namespace

std::string ExportCertPem(const SelfSignedCert& cert) {
    if (!cert.valid || !cert.context) return {};
    const std::string body = Base64WithLineBreaks(cert.context->pbCertEncoded,
                                                    cert.context->cbCertEncoded);
    if (body.empty()) return {};
    return std::string("-----BEGIN CERTIFICATE-----\r\n") + body +
           "-----END CERTIFICATE-----\r\n";
}

std::string ExportPrivKeyPem(const SelfSignedCert& cert) {
    if (!cert.valid || cert.key_name.empty()) return {};

    NCRYPT_PROV_HANDLE prov = 0;
    if (FAILED(NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0))) {
        return {};
    }
    NCRYPT_KEY_HANDLE key = 0;
    if (FAILED(NCryptOpenKey(prov, &key, cert.key_name.c_str(), 0, 0))) {
        NCryptFreeObject(prov);
        return {};
    }

    // Need NCRYPT_EXPORTABLE on the key for PKCS8 export. The key created
    // by CreateSelfSignedCert is exportable by default for ECDSA P-256
    // via MS_KEY_STORAGE_PROVIDER. If the provider denies, ExportKey
    // returns NTE_NOT_SUPPORTED.
    DWORD pkcs8_size = 0;
    SECURITY_STATUS s = NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
                                          nullptr, nullptr, 0,
                                          &pkcs8_size, 0);
    if (s != ERROR_SUCCESS || pkcs8_size == 0) {
        NCryptFreeObject(key);
        NCryptFreeObject(prov);
        return {};
    }
    std::vector<BYTE> pkcs8(pkcs8_size);
    s = NCryptExportKey(key, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
                          nullptr, pkcs8.data(), pkcs8_size, &pkcs8_size, 0);
    NCryptFreeObject(key);
    NCryptFreeObject(prov);
    if (s != ERROR_SUCCESS) return {};
    pkcs8.resize(pkcs8_size);

    const std::string body = Base64WithLineBreaks(pkcs8.data(), pkcs8.size());
    if (body.empty()) return {};
    return std::string("-----BEGIN PRIVATE KEY-----\r\n") + body +
           "-----END PRIVATE KEY-----\r\n";
}

bool WriteCertAndKeyPemFiles(const SelfSignedCert& cert,
                              const std::string& cert_path,
                              const std::string& key_path) {
    const std::string cert_pem = ExportCertPem(cert);
    const std::string key_pem  = ExportPrivKeyPem(cert);
    if (cert_pem.empty() || key_pem.empty()) return false;

    auto write = [](const std::string& path, const std::string& body) {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "wb");
        if (!f) return false;
        const size_t n = std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
        return n == body.size();
    };
    return write(cert_path, cert_pem) && write(key_path, key_pem);
}

}  // namespace lumen
