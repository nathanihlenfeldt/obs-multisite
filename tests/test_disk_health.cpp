// SPDX-License-Identifier: GPL-3.0-or-later
// test_disk_health.cpp — the low-disk readout the encoder dock and the
// appliance's web UI both show.
//
// Pure threshold arithmetic over a free-byte count, kept out of the
// statvfs()/std::filesystem::space() call sites so it can be tested with no
// filesystem at all.
#include "../src/core/disk_health.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("Default thresholds\n");
    {
        DiskThresholds t;
        CHECK(classify_disk_free(50ull * 1024 * 1024 * 1024, t) == DiskHealth::Healthy,
              "50 GiB free is healthy");
        CHECK(classify_disk_free(5ull * 1024 * 1024 * 1024, t) == DiskHealth::Low,
              "exactly the low floor is already Low, not Healthy");
        CHECK(classify_disk_free(2ull * 1024 * 1024 * 1024, t) == DiskHealth::Low,
              "2 GiB free is Low but not yet Critical");
        CHECK(classify_disk_free(1ull * 1024 * 1024 * 1024, t) == DiskHealth::Critical,
              "exactly the critical floor is already Critical");
        CHECK(classify_disk_free(0, t) == DiskHealth::Critical, "no space left is Critical");
    }

    std::printf("A tighter cap (e.g. a small SD card) is configurable\n");
    {
        DiskThresholds t;
        t.low_free_bytes      = 1ull * 1024 * 1024 * 1024;
        t.critical_free_bytes = 256ull * 1024 * 1024;
        CHECK(classify_disk_free(2ull * 1024 * 1024 * 1024, t) == DiskHealth::Healthy,
              "2 GiB is healthy under a tighter cap");
        CHECK(classify_disk_free(512ull * 1024 * 1024, t) == DiskHealth::Low,
              "512 MiB is low under a tighter cap");
        CHECK(classify_disk_free(100ull * 1024 * 1024, t) == DiskHealth::Critical,
              "100 MiB is critical under a tighter cap");
    }

    std::printf("Names used by the UI\n");
    {
        CHECK(std::string(disk_health_name(DiskHealth::Healthy)) == "healthy", "healthy label");
        CHECK(std::string(disk_health_name(DiskHealth::Low)) == "low", "low label");
        CHECK(std::string(disk_health_name(DiskHealth::Critical)) == "critical", "critical label");
    }

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("all disk-health checks passed\n");
    return 0;
}
