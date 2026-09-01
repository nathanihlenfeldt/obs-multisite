#pragma once
//
// crypto.h — tiny platform crypto abstraction for the SigV4 signer.
//
// Two backends, selected at compile time:
//   • Windows  → CNG / bcrypt (built into the OS; no external dependency)
//   • else     → OpenSSL libcrypto (ships with macOS / obs-deps)
//
// This lets the Windows plugin build against nothing but obs-deps' curl +
// FFmpeg — no OpenSSL to locate or ship.
//
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace multisite {
namespace crypto {

// Raw SHA-256 → 32 bytes.
std::vector<uint8_t> sha256(const uint8_t* data, size_t len);

// HMAC-SHA256(key, msg) → 32 bytes.
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                 const std::string& msg);

// Convenience: lowercase hex of SHA-256.
std::string sha256_hex(const uint8_t* data, size_t len);

} // namespace crypto
} // namespace multisite
