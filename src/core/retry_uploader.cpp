#include "retry_uploader.h"
#include <random>
#include <cmath>
#include <algorithm>

namespace multisite {

RetryUploader::RetryUploader(SpoolQueue& spool, Transport& transport,
                             UploaderConfig cfg)
    : m_spool(spool), m_transport(transport), m_cfg(std::move(cfg)) {}

RetryUploader::~RetryUploader() { stop(); }

int RetryUploader::backoff_ms(int attempt) const {
    // exponential: base * 2^(attempt-1), capped, with ± jitter
    double base = (double)m_cfg.base_backoff_ms * std::pow(2.0, std::max(0, attempt - 1));
    double capped = std::min(base, (double)m_cfg.max_backoff_ms);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> d(1.0 - m_cfg.jitter, 1.0 + m_cfg.jitter);
    return (int)std::llround(capped * d(rng));
}

bool RetryUploader::upload_one(const SpooledSegment& seg) {
    int attempt = 0;
    while (m_running) {
        ++attempt;
        PutResult r = m_transport.put(seg.key, seg.data, m_cfg.content_type, m_cfg.tags);
        if (r.success) {
            m_spool.confirm(seg.seq);
            m_stats.confirmed++;
            m_stats.bytes += seg.data.size();
            m_health = LinkHealth::Healthy;
            if (m_on_confirm) m_on_confirm(seg);
            return true;
        }
        if (!r.retryable) {
            // Permanent error (e.g. auth). Don't spin forever on this segment;
            // surface it and stop draining so the operator can fix credentials.
            m_stats.permanent_failures++;
            m_health = LinkHealth::Offline;
            return false;
        }
        m_stats.retries++;
        m_health = (attempt >= 2) ? LinkHealth::Offline : LinkHealth::Degraded;

        if (m_cfg.max_attempts > 0 && attempt >= m_cfg.max_attempts)
            return false;

        int wait = backoff_ms(attempt);
        // sleep in small slices so stop() is responsive
        for (int slept = 0; slept < wait && m_running; slept += 25)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

void RetryUploader::run() {
    while (m_running) {
        auto next = m_spool.peek_next();
        if (!next) {
            m_health = LinkHealth::Healthy;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // Strict in-order: if this one hits a permanent failure, pause the loop
        // briefly rather than reordering past it.
        if (!upload_one(*next) && m_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void RetryUploader::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void RetryUploader::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}

bool RetryUploader::drain_blocking(std::chrono::milliseconds deadline) {
    auto end = std::chrono::steady_clock::now() + deadline;
    bool was_running = m_running.load();
    if (!was_running) m_running = true; // allow upload_one loops
    while (std::chrono::steady_clock::now() < end) {
        auto next = m_spool.peek_next();
        if (!next) { if (!was_running) m_running = false; return true; }
        if (!upload_one(*next)) break; // permanent failure or attempts exhausted
    }
    if (!was_running) m_running = false;
    return m_spool.pending_count() == 0;
}

} // namespace multisite
