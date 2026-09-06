//
// crypto_apple.cpp — CommonCrypto backend (macOS).
//
// Compiled only on Apple platforms, in preference to the OpenSSL backend. The
// reason is distribution, not taste: OpenSSL is not part of macOS, so an
// OpenSSL-linked plugin would either need the library shipped alongside it or
// would fail to load on any Mac without Homebrew. CommonCrypto is in libSystem,
// which every Mac has and every process already links, so the plugin depends on
// nothing a church would have to install.
//
// This mirrors the reasoning behind the Windows CNG backend: use the platform's
// own crypto rather than carrying a dependency for three hash functions.
//
#if defined(__APPLE__)

#include "crypto.h"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>

namespace multisite {
namespace crypto {

std::vector<uint8_t> sha256(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out(CC_SHA256_DIGEST_LENGTH);
    // CC_LONG is 32-bit, so a single call cannot hash more than 4 GB. Nothing
    // here comes close — the largest input is a segment's few megabytes — but
    // stream it rather than truncate silently if that ever changes.
    if (len <= 0xFFFFFFFFu) {
        CC_SHA256(data, (CC_LONG)len, out.data());
    } else {
        CC_SHA256_CTX ctx;
        CC_SHA256_Init(&ctx);
        size_t off = 0;
        while (off < len) {
            const size_t chunk = (len - off > 0x40000000u) ? 0x40000000u
                                                           : (len - off);
            CC_SHA256_Update(&ctx, data + off, (CC_LONG)chunk);
            off += chunk;
        }
        CC_SHA256_Final(out.data(), &ctx);
    }
    return out;
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                 const std::string& msg) {
    std::vector<uint8_t> out(CC_SHA256_DIGEST_LENGTH);
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(),
           msg.data(), msg.size(), out.data());
    return out;
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

#endif // __APPLE__
