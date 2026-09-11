// SPDX-License-Identifier: GPL-3.0-or-later
// test_position_interp.cpp — the playhead between state samples.
//
// This exists because the first attempt at smoothing the dock's playhead
// shipped a frozen timer and an apparently dead Play button, and neither the
// status API nor a stack sample could see anything wrong: the backend was
// healthy and the refresh was running. The display was simply pinned to a
// clamped constant by a faster timer that always had the last word.
//
// The structural fix is one writer, which is a shape a unit test cannot check.
// What it CAN check is the arithmetic's two safety properties — that
// extrapolation is bounded relative to its own sample, and that the absolute
// bound can only cap rather than become the answer — so those are pinned here.
#include "../src/core/position_interp.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    const long long kBase = 1'700'000'000'000LL;   // a playhead, in wall ms
    const long long kWall = 5'000'000LL;           // when it was sampled
    const long long kMaxExtrap = 750;

    std::printf("== between samples, it advances 1:1 with wall time ==\n");
    CHECK(interpolate_position(kBase, kWall, kWall, 0, kMaxExtrap) == kBase,
          "at the sample instant it IS the sample");
    CHECK(interpolate_position(kBase, kWall, kWall + 100, 0, kMaxExtrap)
              == kBase + 100,
          "100ms later it reads 100ms further on");
    CHECK(interpolate_position(kBase, kWall, kWall + 499, 0, kMaxExtrap)
              == kBase + 499,
          "just before the next refresh it has covered the whole gap");

    std::printf("== it never runs away from its own sample ==\n");
    {
        // The failure this guards: refreshes stop, and the readout is left
        // free-running. It must settle just past the last real number.
        const long long after10s =
            interpolate_position(kBase, kWall, kWall + 10'000, 0, kMaxExtrap);
        CHECK(after10s == kBase + kMaxExtrap,
              "10s with no refresh settles at base + the extrapolation cap");
        const long long afterAnHour =
            interpolate_position(kBase, kWall, kWall + 3'600'000, 0, kMaxExtrap);
        CHECK(afterAnHour == after10s,
              "an hour with no refresh is no worse than ten seconds");
    }

    std::printf("== a backwards clock does not rewind the playhead ==\n");
    CHECK(interpolate_position(kBase, kWall, kWall - 5'000, 0, kMaxExtrap) == kBase,
          "NTP stepping the clock back holds at the sample");

    std::printf("== the bound caps, and cannot become the answer ==\n");
    {
        // The shape of the original bug: an absolute bound that the readout got
        // stuck on. It may only ever trim the extrapolation.
        const long long bound = kBase + 200;
        CHECK(interpolate_position(kBase, kWall, kWall + 500, bound, kMaxExtrap)
                  == bound,
              "extrapolation past the end of a recording stops at the end");
        CHECK(interpolate_position(kBase, kWall, kWall, bound, kMaxExtrap) == kBase,
              "at the sample instant the bound does not drag it forward");
        // The one that actually broke: a bound BELOW the sample must not pull
        // the readout backwards to it and hold it there.
        const long long staleBound = kBase - 60'000;
        CHECK(interpolate_position(kBase, kWall, kWall + 100, staleBound, kMaxExtrap)
                  == staleBound,
              "a stale bound below the sample clamps (and is visibly wrong)");
        CHECK(interpolate_position(kBase + 120'000, kWall, kWall + 100,
                                   staleBound, kMaxExtrap) == staleBound,
              "…but the NEXT sample is what corrects it, not the clamp");
    }

    std::printf("== no bound means no cap ==\n");
    CHECK(interpolate_position(kBase, kWall, kWall + 400, 0, kMaxExtrap)
              == kBase + 400,
          "bound 0 is 'unbounded', not 'clamp to zero'");

    std::printf("\n%s\n", g_fail == 0 ? "ALL POSITION INTERP TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
