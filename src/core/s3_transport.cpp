#include "s3_transport.h"
#include "aws_sigv4.h"

#include <curl/curl.h>

#include <cstring>
#include <mutex>
#include <sstream>
#include <algorithm>

namespace multisite {

static std::once_flag g_curl_once;
static void ensure_curl() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static size_t write_to_vec(void* ptr, size_t sz, size_t nm, void* ud) {
    auto* v = static_cast<std::vector<uint8_t>*>(ud);
    size_t n = sz * nm;
    const uint8_t* p = static_cast<const uint8_t*>(ptr);
    v->insert(v->end(), p, p + n);
    return n;
}

struct ReadCtx { const uint8_t* data; size_t size; size_t pos; };
static size_t read_from_buf(void* dest, size_t sz, size_t nm, void* ud) {
    auto* rc = static_cast<ReadCtx*>(ud);
    size_t want = sz * nm, rem = rc->size - rc->pos;
    size_t n = std::min(want, rem);
    if (n) { std::memcpy(dest, rc->data + rc->pos, n); rc->pos += n; }
    return n;
}

struct S3Transport::Impl {
    S3Config cfg;

    std::string host() const {
        if (!cfg.endpoint_host.empty()) return cfg.endpoint_host;
        return cfg.r2_account_id + ".r2.cloudflarestorage.com";
    }
    std::string url_for(const std::string& key) const {
        return std::string(cfg.use_https ? "https://" : "http://") +
               host() + "/" + cfg.bucket + "/" + key;
    }

    // x-amz-tagging value: url-encoded key=value pairs joined by &
    static std::string tag_header(const std::map<std::string, std::string>& tags) {
        std::string out;
        for (const auto& [k, v] : tags) {
            if (!out.empty()) out += "&";
            out += SigV4Signer::uri_encode(k, true) + "=" +
                   SigV4Signer::uri_encode(v, true);
        }
        return out;
    }

    PutResult do_put(const std::string& key, const std::vector<uint8_t>& body,
                     const std::string& content_type,
                     const std::map<std::string, std::string>& tags) {
        ensure_curl();
        PutResult res;
        CURL* curl = curl_easy_init();
        if (!curl) { res.error = "curl_easy_init failed"; return res; }

        std::string url = url_for(key);

        std::map<std::string, std::string> extra;
        if (!content_type.empty()) extra["Content-Type"] = content_type;
        // Retention tagging + CDN cache hint. NOTE: Cloudflare R2 rejects
        // x-amz-tagging, so the header is only sent when tags are supplied.
        std::string tg = tag_header(tags);
        if (!tg.empty()) extra["x-amz-tagging"] = tg;
        extra["Cache-Control"] = "max-age=604800";

        SigV4Signer signer(cfg.access_key_id, cfg.secret_access_key,
                           cfg.region, "s3");
        auto signed_req = signer.sign("PUT", url, body, extra);

        struct curl_slist* headers = nullptr;
        for (const auto& line : signed_req.header_lines())
            headers = curl_slist_append(headers, line.c_str());

        std::vector<uint8_t> resp;
        ReadCtx rc{ body.data(), body.size(), 0 };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_from_buf);
        curl_easy_setopt(curl, CURLOPT_READDATA, &rc);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)body.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)cfg.connect_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.request_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        CURLcode cc = curl_easy_perform(curl);
        if (cc == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            res.http_status = code;
            res.success = (code >= 200 && code < 300);
            if (!res.success) {
                // 4xx (except 408/429) are permanent: bad creds, bad bucket,
                // bad request. Retrying those forever would mask a config error.
                res.retryable = !(code >= 400 && code < 500) ||
                                code == 408 || code == 429;
                std::string bodytxt(resp.begin(),
                    resp.begin() + std::min<size_t>(resp.size(), 400));
                res.error = "HTTP " + std::to_string(code) + " " + bodytxt;
            }
        } else {
            // Network-level failure → retryable (this is the outage case).
            res.retryable = true;
            res.error = curl_easy_strerror(cc);
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return res;
    }

    // GET used only by self_test.
    PutResult do_get(const std::string& key, std::vector<uint8_t>& out) {
        ensure_curl();
        PutResult res;
        CURL* curl = curl_easy_init();
        if (!curl) { res.error = "curl init"; return res; }
        std::string url = url_for(key);
        SigV4Signer signer(cfg.access_key_id, cfg.secret_access_key, cfg.region, "s3");
        auto sr = signer.sign("GET", url, {}, {});
        struct curl_slist* h = nullptr;
        for (const auto& l : sr.header_lines()) h = curl_slist_append(h, l.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)cfg.request_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        CURLcode cc = curl_easy_perform(curl);
        if (cc == CURLE_OK) {
            long code = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            res.http_status = code;
            res.success = (code >= 200 && code < 300);
            if (!res.success) res.error = "HTTP " + std::to_string(code);
        } else res.error = curl_easy_strerror(cc);
        curl_slist_free_all(h);
        curl_easy_cleanup(curl);
        return res;
    }
};

S3Transport::S3Transport(S3Config cfg) : d(std::make_unique<Impl>()) {
    d->cfg = std::move(cfg);
}
S3Transport::~S3Transport() = default;

std::string S3Transport::host() const { return d->host(); }

PutResult S3Transport::put(const std::string& key,
                           const std::vector<uint8_t>& body,
                           const std::string& content_type,
                           const std::map<std::string, std::string>& tags) {
    return d->do_put(key, body, content_type, tags);
}

std::string S3Transport::self_test() {
    const std::string key = "_multisite_probe.txt";
    std::string payload = "obs-multisite connectivity probe";
    std::vector<uint8_t> body(payload.begin(), payload.end());

    auto p = d->do_put(key, body, "text/plain", { {"MultisiteExpiry", "7d"} });
    if (!p.success) return "write failed: " + p.error;

    std::vector<uint8_t> got;
    auto g = d->do_get(key, got);
    if (!g.success) return "read-back failed: " + g.error;
    if (got.size() != body.size())
        return "read-back mismatch (wrote " + std::to_string(body.size()) +
               " bytes, read " + std::to_string(got.size()) + ")";
    return "";
}

} // namespace multisite
