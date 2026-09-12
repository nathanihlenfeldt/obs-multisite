// SPDX-License-Identifier: GPL-3.0-or-later
#include "disk_health.h"

namespace multisite {

DiskHealth classify_disk_free(uint64_t free_bytes, const DiskThresholds& t) {
    if (free_bytes <= t.critical_free_bytes) return DiskHealth::Critical;
    if (free_bytes <= t.low_free_bytes)       return DiskHealth::Low;
    return DiskHealth::Healthy;
}

const char* disk_health_name(DiskHealth h) {
    switch (h) {
        case DiskHealth::Critical: return "critical";
        case DiskHealth::Low:      return "low";
        default:                  return "healthy";
    }
}

} // namespace multisite
