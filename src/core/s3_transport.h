#pragma once
//
// s3_transport.h — the production Transport: libcurl + the validated SigV4
// signer, against any S3-compatible endpoint (Cloudflare R2, AWS S3, MinIO,
// Backblaze B2, Wasabi).
//
#include "transport.h"
#include <memory>
#include <string>

namespace multisite {

struct S3Config {
    // Either give a full endpoint host, or an R2 account id (which builds the
    // standard R2 endpoint automatically).
    std::string endpoint_host;      // e.g. "s3.us-east-1.amazonaws.com" / "minio.local:9000"
    std::string r2_account_id;      // e.g. "abc123" → abc123.r2.cloudflarestorage.com
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";    // "auto" for R2; real region for AWS
    bool        use_https = true;
    int         connect_timeout_ms = 5000;
    int         request_timeout_ms = 30000;  // segments can be large
};

class S3Transport : public Transport {
public:
    explicit S3Transport(S3Config cfg);
    ~S3Transport() override;

    PutResult put(const std::string& key,
                  const std::vector<uint8_t>& body,
                  const std::string& content_type,
                  const std::map<std::string, std::string>& tags) override;

    // HEAD the object and return Content-Length, or -1 if missing/unreachable.
    int64_t object_size(const std::string& key) override;

    // Signed GET. Used by the decoder to fetch manifests and segments.
    GetResult get(const std::string& key) override;

    // Simple connectivity/credential check: PUT then GET a tiny probe object.
    // Returns an empty string on success, or a human-readable error.
    std::string self_test();

    std::string host() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite
