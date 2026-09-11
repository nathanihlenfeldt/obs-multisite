// SPDX-License-Identifier: GPL-3.0-or-later
// test_playout_clock.cpp — the arithmetic that decides when a frame is due.
//
// Written while chasing a lipsync report, to test a theory that turned out
// to be WRONG. The theory: the four call sites computed a frame's due time
// as `base + (uint64_t)(pts - first)`, and since the anchor race means the
// losing stream delivers frames with a pts EARLIER than the anchor, those
// negative deltas looked like they would wrap to ~1.8e19 and hand the output
// a garbage timestamp.
//
// They do not. Unsigned overflow is modular: the wrap in the cast cancels
// the wrap in the addition, so the unsigned and signed forms are
// bit-identical for every value that can actually occur. The cast was never
// the bug, and the lipsync cause is still open.
//
// The test is kept because it now pins that equivalence — so the next person
// to find that cast suspicious can read this instead of spending the evening
// re-deriving it — along with the single case where the two forms genuinely
// differ: a frame due before the epoch, which clamps rather than wrapping.
#include "../src/core/playout_clock.h"

#include <cstdint>
#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

int main() {
    // A 500ms cushion, as anchor_pts sets up.
    const uint64_t kCushion = 500000000ULL;
    const uint64_t base = 1000000000ULL + kCushion;   // 1s + cushion

    std::printf("== a frame at the anchor is due exactly at the base ==\n");
    CHECK(playout_due_ns(base, 5000, 5000) == base,
          "zero offset lands on the base");

    std::printf("== a frame after the anchor is due later, 1:1 ==\n");
    CHECK(playout_due_ns(base, 5000 + 250000000LL, 5000) == base + 250000000ULL,
          "250ms after the anchor is due 250ms after the base");

    std::printf("== a frame BEFORE the anchor uses the cushion ==\n");
    {
        // The realistic case: the other stream's frame, 344ms earlier than
        // the anchor. It must land 344ms BEFORE the base, which the cushion
        // exists to allow. NOTE: the old unsigned form passes this too —
        // that is the point, and why it was not the bug.
        const int64_t interleave = 344000000LL;       // 344ms, as measured
        const uint64_t due = playout_due_ns(base, 5000 - interleave, 5000);
        CHECK(due == base - (uint64_t)interleave,
              "344ms before the anchor is due 344ms before the base");
        // Sanity: genuinely earlier, and still schedulable.
        CHECK(due < base, "it is genuinely earlier than the base, not wrapped");
        CHECK(due > 1000000000ULL,
              "and still a sane, schedulable moment rather than a wrapped one");
    }

    std::printf("== the cushion is not infinite: clamp, never wrap ==\n");
    {
        // The ONE case where the signed and unsigned forms differ, and the
        // only assertion here that the old arithmetic fails. It needs a delta
        // more negative than the base, so it cannot arise while the base is
        // nanoseconds of uptime — kept as a boundary, not as a live fault.
        const uint64_t due = playout_due_ns(1000, 0, 5000000000LL);
        CHECK(due == 0, "a frame due before the epoch clamps to zero");
    }

    std::printf("== a whole interleave window of frames stays ordered ==\n");
    {
        // The real-world shape: audio won the anchor race, and the video
        // frames of that fragment arrive spanning from before it to after.
        // Every one of them must come out monotonically increasing — that
        // ordering is what sync actually is.
        const int64_t anchor = 10000000000LL;          // 10s
        uint64_t previous = 0;
        bool ordered = true;
        for (int frame = -10; frame <= 10; ++frame) {  // ±333ms at 30fps
            const int64_t pts = anchor + (int64_t)frame * 33333333LL;
            const uint64_t due = playout_due_ns(base, pts, anchor);
            if (previous != 0 && due <= previous) ordered = false;
            previous = due;
        }
        CHECK(ordered, "frames either side of the anchor stay in order");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PLAYOUT CLOCK TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
