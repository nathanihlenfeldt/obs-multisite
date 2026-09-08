// SPDX-License-Identifier: GPL-3.0-or-later
// test_storage_health.cpp — reading what the store says about itself.
//
// These are the figures an operator is shown when a campus stutters, so being
// wrong here means sending somebody to look in the wrong place. The parsing
// and the arithmetic are kept out of the transport precisely so they can be
// tested without a network, a bucket or libcurl.
#include "../src/core/storage_health.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    std::printf("The Cloudflare colo, from cf-ray\n");
    {
        CHECK(cloudflare_colo("7d3f1a2b3c4d5e6f-JNB") == "JNB",
              "Johannesburg is read from a real cf-ray");
        CHECK(cloudflare_colo("7d3f1a2b3c4d5e6f-CPT") == "CPT",
              "so is Cape Town");
        CHECK(cloudflare_colo("  7d3f1a2b3c4d5e6f-AMS  ") == "AMS",
              "surrounding whitespace does not defeat it");
        CHECK(cloudflare_colo("7d3f1a2b3c4d5e6f-jnb") == "JNB",
              "a lowercase code is normalised, not rejected");

        // A store that is not Cloudflare must produce nothing rather than a
        // plausible-looking three letters, or the interface would claim a
        // colo for MinIO and AWS.
        CHECK(cloudflare_colo("") == "", "an absent header gives nothing");
        CHECK(cloudflare_colo("7d3f1a2b3c4d5e6f") == "",
              "an id with no colo suffix gives nothing");
        CHECK(cloudflare_colo("req-12345678") == "",
              "a numeric suffix is not a colo");
        CHECK(cloudflare_colo("something-TOOLONGHERE") == "",
              "an over-long suffix is not a colo");
        CHECK(cloudflare_colo("trailing-") == "",
              "a dangling dash does not crash or invent one");
    }

    std::printf("Matching header names\n");
    {
        std::string v;
        CHECK(header_is("cf-ray: abc-JNB\r\n", "cf-ray", v) && v == "abc-JNB",
              "the value is taken and the CRLF trimmed");
        CHECK(header_is("CF-RAY: abc-JNB\r\n", "cf-ray", v),
              "matching is case-insensitive, as HTTP requires");
        v.clear();
        CHECK(!header_is("cf-ray-extra: no\r\n", "cf-ray", v),
              "a longer header name is not a match");
        CHECK(!header_is("server: cloudflare\r\n", "cf-ray", v),
              "an unrelated header is not a match");
        CHECK(header_is("server: cloudflare\r\n", "server", v) &&
              v == "cloudflare", "and the one asked for is");
    }

    std::printf("The observed rate\n");
    {
        RateMeter m;
        CHECK(!m.has_measurement(), "reports nothing before it has measured");
        CHECK(m.bytes_per_s() == 0.0, "and no rate");

        // A 2 MiB segment in one second: 2 MiB/s, which is 2097152 and not
        // 2000000 — the figures are bytes, so the test says so too.
        const double two_mib = 2.0 * 1024 * 1024;
        m.add(2u * 1024 * 1024, 1.0);
        CHECK(m.has_measurement(), "one transfer is a measurement");
        CHECK(m.bytes_per_s() > two_mib * 0.99 && m.bytes_per_s() < two_mib * 1.01,
              "the first sample is taken as-is rather than smoothed from zero");

        // Small or quick transfers say more about latency than capacity, and
        // letting them in made the figure swing between segments.
        const double before = m.bytes_per_s();
        m.add(200, 0.004);
        CHECK(m.bytes_per_s() == before, "a tiny fetch is ignored");
        m.add(4u * 1024 * 1024, 0.001);
        CHECK(m.bytes_per_s() == before, "so is one too quick to time");

        // A slow fetch moves the figure without dominating it: this is what
        // keeps a display steady while still showing a link going bad.
        m.add(1u * 1024 * 1024, 4.0);          // 256 kB/s
        CHECK(m.bytes_per_s() < before, "a slower transfer brings it down");
        CHECK(m.bytes_per_s() > 256.0e3,
              "but one sample does not drag it all the way");
        CHECK(m.samples() == 2, "and only real measurements are counted");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL STORAGE HEALTH TESTS PASSED"
                                      : "STORAGE HEALTH TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
