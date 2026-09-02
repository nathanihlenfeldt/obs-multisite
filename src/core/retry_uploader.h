#pragma once
//
// retry_uploader.h — drains the durable spool to the object store in strict
// sequence order, retrying with exponential backoff + jitter. No segment is
// abandoned while the event is live; a segment is confirmed (and its spool file
// removed) only after the store returns success.
//
#include "spool_queue.h"
#include "transport.h"
#include <functional>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdint>

namespace multisite {

enum class LinkHealth { Healthy, Degraded, Offline };

struct UploaderConfig {
    int    base_backoff_ms = 250;     // first retry delay
    int    max_backoff_ms  = 15000;   // cap
    double jitter          = 0.30;    // ±30%
    int    max_attempts    = 0;       // 0 = retry forever (production)
    std::string content_type = "video/mp4";
    // Verify (via HEAD) that the first N successful uploads really persisted,
    // with the expected byte count. Catches a store that returns 2xx without
    // storing, and mis-signed requests. 0 disables.
    int verify_first_n = 3;
    std::map<std::string, std::string> tags = { {"MultisiteExpiry", "7d"} };
};

struct UploaderStats {
    std::atomic<uint64_t> confirmed{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> retries{0};
    std::atomic<uint64_t> permanent_failures{0};
    std::atomic<uint64_t> verify_failures{0};
};

// Called after each segment is confirmed durable, so the caller can update the
// manifest (honouring the write-ordering rule: manifest only lists confirmed
// segments).
using ConfirmCallback = std::function<void(const SpooledSegment&)>;

class RetryUploader {
public:
    RetryUploader(SpoolQueue& spool, Transport& transport, UploaderConfig cfg = {});
    ~RetryUploader();

    void set_confirm_callback(ConfirmCallback cb) { m_on_confirm = std::move(cb); }
    // Fires after the spool entry is cleared, so status() counters are accurate.
    void set_post_confirm_callback(ConfirmCallback cb) { m_on_confirmed_after = std::move(cb); }

    void start();
    void stop();

    LinkHealth health() const { return m_health.load(); }
    const std::string& last_verify_note() const { return m_last_verify_note; }
    const UploaderStats& stats() const { return m_stats; }

    // Drain synchronously until the spool is empty or `deadline` passes. Returns
    // true if fully drained. Used by tests and by clean shutdown.
    bool drain_blocking(std::chrono::milliseconds deadline);

private:
    SpoolQueue&    m_spool;
    Transport&     m_transport;
    UploaderConfig m_cfg;
    UploaderStats  m_stats;
    ConfirmCallback m_on_confirm;
    ConfirmCallback m_on_confirmed_after;

    std::string          m_last_verify_note;
    std::thread          m_thread;
    std::atomic<bool>    m_running{false};
    std::atomic<LinkHealth> m_health{LinkHealth::Healthy};

    void run();
    // Upload one segment with retry until success/stop. Returns true on confirm.
    bool upload_one(const SpooledSegment& seg);
    int  backoff_ms(int attempt) const;
};

} // namespace multisite
