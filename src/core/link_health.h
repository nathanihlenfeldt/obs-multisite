// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// link_health.h — a small, testable state machine for "is the connection to
// the object store healthy right now?".
//
// The encoder, the decoder and the appliance all need the same answer to the
// same question — and, crucially, they need to tell a *broken connection* apart
// from *nothing to fetch*. A request that reaches the store (200, 403, 404,
// even a well-formed error page) proves the link is up; only a transport-level
// failure (timeout, DNS, connection refused, TLS, 5xx) counts against it.
//
// Kept free of libcurl and of any particular transport so the arithmetic is
// covered by the ordinary test suite, exactly like storage_health.h.
//
#include <atomic>
#include <cstdint>

namespace multisite {

// Three states, ordered from best to worst. The encoder has used these words
// since the beginning; the satellites adopt them so one operator sees the same
// meaning everywhere.
enum class LinkHealth { Healthy, Degraded, Offline };

// One shared rule for every end. `observe()` is fed the outcome of every
// network request that touches the store; the tracker applies the same
// hysteresis the encoder's uploader already did: any success resets to
// Healthy, the first failure reads as Degraded, and a second consecutive
// failure reads as Offline. Recovery to Healthy happens the moment one
// request gets through again.
class LinkTracker {
public:
    // `ok` is whether the last request reached the store and got a response
    // (any HTTP status) back. A request that never reached the store is not ok.
    // `now_override` exists for deterministic tests; 0 means "the real clock".
    void observe(bool ok, int64_t now_override = 0);

    LinkHealth health() const { return m_health.load(); }
    // True once anything has been observed at all, so a UI can tell "measured
    // healthy" from "no reading yet".
    bool known() const { return m_known.load(); }
    // Number of consecutive failures currently recorded. 0 when healthy.
    uint64_t consecutive_failures() const { return m_failures.load(); }
    // Wall-clock ms (multisite::now_ms) of the last time the health changed
    // state, or 0 before anything has been observed.
    int64_t last_change_ms() const { return m_changed_ms.load(); }

private:
    std::atomic<bool>       m_known{false};
    std::atomic<LinkHealth> m_health{LinkHealth::Healthy};
    std::atomic<uint64_t>   m_failures{0};
    std::atomic<int64_t>    m_changed_ms{0};
};

// Classify a get()/put()/list() result into "the store answered" vs "the link
// is broken". A 0 status means libcurl never completed a request; 5xx means the
// store (or a gateway in front of it) is failing. Everything else reached the
// store and got a reply, even if that reply was 404 or 403.
inline bool link_reachable_result(bool success, long http_status) {
    if (success) return true;
    return http_status != 0 && http_status < 500;
}

} // namespace multisite
