// SPDX-License-Identifier: GPL-3.0-or-later
#include "link_health.h"
#include "session.h"   // for now_ms()

namespace multisite {

void LinkTracker::observe(bool ok, int64_t now_override) {
    const int64_t now = now_override ? now_override : now_ms();

    uint64_t failures;
    LinkHealth next;
    if (ok) {
        failures = 0;
        next = LinkHealth::Healthy;
    } else {
        failures = m_failures.load() + 1;
        next = (failures >= 2) ? LinkHealth::Offline : LinkHealth::Degraded;
    }

    m_failures.store(failures);
    const LinkHealth prev = m_health.load();
    m_health.store(next);
    m_known.store(true);
    if (prev != next) m_changed_ms.store(now);
}

} // namespace multisite
