#pragma once
//
// checksum.h — SHA-256 helpers used for segment integrity (recorded in the
// manifest, verified by decoders on download).
//
#include "crypto.h"
#include <string>
#include <vector>
#include <cstdint>

namespace multisite {

inline std::string sha256_hex(const std::vector<uint8_t>& data) {
    return crypto::sha256_hex(data.data(), data.size());
}

inline bool verify_sha256(const std::vector<uint8_t>& data,
                          const std::string& expected_hex) {
    return !expected_hex.empty() && sha256_hex(data) == expected_hex;
}

} // namespace multisite
