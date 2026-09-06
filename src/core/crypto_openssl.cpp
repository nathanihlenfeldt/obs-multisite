//
// crypto_openssl.cpp — OpenSSL backend (Linux, and any Unix that is not macOS).
//
// Windows uses CNG and macOS uses CommonCrypto, both part of the OS. This is
// the backend for platforms where the system crypto is OpenSSL anyway.
//
#if !defined(_WIN32) && !defined(__APPLE__)

#include "crypto.h"
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

namespace multisite {
namespace crypto {

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(SHA256_DIGEST_LENGTH);
    SHA256(data, len, out.data());
    return out;
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                 const std::string& msg) {
    unsigned int n = 0;
    unsigned char buf[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         buf, &n);
    return std::vector<uint8_t>(buf, buf + n);
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

#endif // !_WIN32 && !__APPLE__
