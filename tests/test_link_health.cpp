// SPDX-License-Identifier: GPL-3.0-or-later
// test_link_health.cpp — the three-state connection readout.
//
// This is what tells an operator, mid-service, that their venue's internet has
// gone while the buffered segments are still playing out. The arithmetic is
// kept in the core so it can be tested without a network, a bucket or libcurl.
#include "../src/core/link_health.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("Classifying a result as \"reached the store\" or not\n");
    {
        CHECK(link_reachable_result(true, 200), "success is reachable");
        CHECK(link_reachable_result(true, 0), "success with no status is reachable");
        CHECK(link_reachable_result(false, 404),
              "a 404 reached the store, so the link is up");
        CHECK(link_reachable_result(false, 403),
              "a 403 reached the store too — auth is not an outage");
        CHECK(!link_reachable_result(false, 0),
              "status 0 means the request never completed");
        CHECK(!link_reachable_result(false, 503),
              "a 5xx is the store (or a gateway) failing");
        CHECK(!link_reachable_result(false, 500), "so is a plain 500");
    }

    std::printf("The tracker's hysteresis\n");
    {
        LinkTracker t;
        CHECK(t.health() == LinkHealth::Healthy, "starts healthy");
        CHECK(t.consecutive_failures() == 0, "with no failures yet");
        CHECK(t.last_change_ms() == 0, "and no transition time yet");
        CHECK(!t.known(), "nothing has been measured yet");

        t.observe(false, 1000);
        CHECK(t.known(), "the first observation makes it known");
        CHECK(t.health() == LinkHealth::Degraded,
              "one failure degrades, it does not cry wolf");
        CHECK(t.consecutive_failures() == 1, "and counts it");
        CHECK(t.last_change_ms() == 1000, "the transition is timestamped");

        t.observe(false, 2000);
        CHECK(t.health() == LinkHealth::Offline,
              "two consecutive failures are offline");
        CHECK(t.consecutive_failures() == 2, "both counted");
        CHECK(t.last_change_ms() == 2000,
              "only a real change updates the timestamp");

        t.observe(false, 3000);
        CHECK(t.health() == LinkHealth::Offline, "stays offline while failing");
        CHECK(t.last_change_ms() == 2000,
              "an unchanged state does not move the timestamp");

        t.observe(true, 4000);
        CHECK(t.health() == LinkHealth::Healthy,
              "one success recovers straight back to healthy");
        CHECK(t.consecutive_failures() == 0, "and the count resets");
        CHECK(t.last_change_ms() == 4000, "recovery is a transition too");

        t.observe(true, 5000);
        CHECK(t.health() == LinkHealth::Healthy, "stays healthy while fine");
        CHECK(t.last_change_ms() == 4000,
              "healthy-to-healthy is not a transition");
    }

    std::printf("A wobble, not an outage\n");
    {
        LinkTracker t;
        t.observe(false, 1000);   // degraded
        t.observe(true, 2000);    // recovered before it became offline
        CHECK(t.health() == LinkHealth::Healthy,
              "a single recovered blip leaves the link healthy");
        t.observe(false, 3000);
        CHECK(t.health() == LinkHealth::Degraded,
              "the count restarts from the last failure, not from history");
    }

    if (g_fail) {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("all link-health checks passed\n");
    return 0;
}
