// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// s3_transport.h — the production Transport: libcurl + the validated SigV4
// signer, against any S3-compatible endpoint (Cloudflare R2, AWS S3, MinIO,
// Backblaze B2, Wasabi).
//
#include "transport.h"
#include "storage_health.h"
#include <memory>
#include <string>

namespace multisite {

// The answer to "is the bucket reachable, and from where?" — one read-only
// request, so a satellite's read-only credentials are enough. self_test()
// cannot serve here: it writes a probe object, which a decoder key is rightly
// forbidden from doing, so it reports a permissions failure as a fault.
struct StorageProbe {
    bool        reachable = false;   // the endpoint answered at all
    bool        readable = false;    // …and gave us the object
    long        http_status = 0;
    std::string error;               // empty when readable
    int64_t     round_trip_ms = 0;
    // Cloudflare's edge that served it, e.g. "JNB". Empty for other stores.
    std::string colo;
    std::string server;              // the Server header, verbatim
    std::string endpoint;            // host asked, for the display
};

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

    // Signed ListObjectsV2. Used to enumerate events for the event list.
    ListResult list(const std::string& prefix,
                    const std::string& delimiter = "",
                    const std::string& continuation_token = "",
                    int max_keys = 1000) override;

    // Simple connectivity/credential check: PUT then GET a tiny probe object.
    // Returns an empty string on success, or a human-readable error.
    // Encoder-side only — it writes, so a read-only key fails it by design.
    std::string self_test();

    // Read-only reachability check, for a satellite or the relay. `key` should
    // be something the site expects to exist — a room's live.json — so a 404
    // is meaningful rather than expected.
    StorageProbe probe(const std::string& key);

    // Where the last response came from, and how fast the link has been.
    // Populated by ordinary traffic, so during a service these reflect the
    // real segment fetches rather than a synthetic test.
    std::string last_colo() const;
    std::string last_server() const;
    // Smoothed observed throughput in bytes per second, and how many
    // transfers large enough to be worth timing have contributed. 0 samples
    // means no figure should be shown rather than a zero rate.
    //
    // Up and down are separate because a site only ever does one of them, and
    // they answer different questions: a main site needs to know what its
    // upload is managing, a campus whether it can bank a buffer.
    double   observed_upload_bytes_per_s() const;
    uint64_t upload_samples() const;
    double   observed_download_bytes_per_s() const;
    uint64_t download_samples() const;

    std::string host() const;

    // The resolved base URL (scheme + host + bucket), for logging. No secrets.
    std::string base_url() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite
