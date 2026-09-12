// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// disk_health.h — is the disk under a cache/spool directory running out.
//
// Deliberately just arithmetic over numbers the caller already has (from
// std::filesystem::space() on the encoder/OBS side, statvfs on the appliance
// side) — the same split storage_health.h uses for the network: the syscall
// is platform-specific and lives at the call site, the threshold logic is
// portable and belongs here, where it is covered by the ordinary test suite.
//
#include <cstdint>

namespace multisite {

enum class DiskHealth { Healthy, Low, Critical };

struct DiskThresholds {
    // Absolute free-space floors, not a percentage of the drive. A cache or
    // spool fills at roughly a constant bytes-per-hour rate regardless of how
    // big the disk under it is, so "how much longer until it's full" is a far
    // steadier signal than "what fraction is left" — a multi-terabyte editing
    // drive at 90% full is not remotely the same situation as a 32 GB SD card
    // at 90% full.
    uint64_t low_free_bytes      = 5ull  * 1024 * 1024 * 1024; // 5 GiB
    uint64_t critical_free_bytes = 1ull  * 1024 * 1024 * 1024; // 1 GiB
};

DiskHealth classify_disk_free(uint64_t free_bytes,
                              const DiskThresholds& t = {});

// Human-readable label for a UI, in the same style as LinkHealth's text keys.
const char* disk_health_name(DiskHealth h);

} // namespace multisite
