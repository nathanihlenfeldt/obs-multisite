#pragma once
//
// transport.h — the object-store PUT abstraction the uploader drains through.
//
// The real implementation wraps libcurl + the SigV4 signer against R2/S3. A
// fault-injecting mock implements the same interface so retry/backoff and
// store-and-forward behaviour can be tested without a network.
//
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace multisite {

struct PutResult {
    bool        success = false;
    long        http_status = 0;
    bool        retryable = true;   // false for permanent errors (e.g. 403)
    std::string error;
};

// Abstract object-store transport.
class Transport {
public:
    virtual ~Transport() = default;
    // PUT an object. `tags` become x-amz-tagging (e.g. MultisiteExpiry=7d).
    virtual PutResult put(const std::string& key,
                          const std::vector<uint8_t>& body,
                          const std::string& content_type,
                          const std::map<std::string, std::string>& tags) = 0;
};

} // namespace multisite
