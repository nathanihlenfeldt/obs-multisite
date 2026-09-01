//
// crypto_bcrypt.cpp — Windows CNG (bcrypt) backend.
// Compiled only when targeting Windows. Links against bcrypt.lib, which is
// part of Windows itself, so the plugin needs no OpenSSL on Windows.
//
#if defined(_WIN32)

#include "crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

namespace multisite {
namespace crypto {

namespace {

// One-shot hash/HMAC using CNG. If hmac_key is non-null, computes HMAC-SHA256;
// otherwise a plain SHA-256. Returns 32 bytes.
std::vector<uint8_t> cng_digest(const uint8_t* data, size_t len,
                                const uint8_t* hmac_key, size_t key_len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> out(32);

    DWORD flags = hmac_key ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM,
                                              nullptr, flags);
    if (st != STATUS_SUCCESS) throw std::runtime_error("BCryptOpenAlgorithmProvider");

    // key/secret is passed to BCryptCreateHash (null for plain SHA-256).
    st = BCryptCreateHash(hAlg, &hHash, nullptr, 0,
                          const_cast<PUCHAR>(hmac_key),
                          (ULONG)key_len, 0);
    if (st != STATUS_SUCCESS) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error("BCryptCreateHash");
    }

    st = BCryptHashData(hHash, const_cast<PUCHAR>(data), (ULONG)len, 0);
    if (st == STATUS_SUCCESS)
        st = BCryptFinishHash(hHash, out.data(), (ULONG)out.size(), 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st != STATUS_SUCCESS) throw std::runtime_error("BCrypt hash/finish");
    return out;
}

} // namespace

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    return cng_digest(data, len, nullptr, 0);
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                 const std::string& msg) {
    return cng_digest(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(),
                      key.data(), key.size());
}

std::string sha256_hex(const uint8_t* data, size_t len) {
    auto h = sha256(data, len);
    static const char* hx = "0123456789abcdef";
    std::string s;
    s.reserve(h.size() * 2);
    for (uint8_t b : h) { s.push_back(hx[b >> 4]); s.push_back(hx[b & 0xF]); }
    return s;
}

} // namespace crypto
} // namespace multisite

#endif // _WIN32
