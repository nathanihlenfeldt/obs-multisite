// SPDX-License-Identifier: GPL-3.0-or-later
// test_timebase.cpp — the segment-duration estimate that every displayed time
// is built on.
//
// Manifest::stream_duration_hint() maps sequence numbers onto clock times for
// every segment outside the manifest's rolling window, so it decides the
// timeline axis, "behind live", the buffered and rewindable figures, and a
// recording's total length. It used to return the LAST listed segment's
// duration, and the last segment of a finished recording is the partial
// fragment the broadcast ended on — the one sample guaranteed to be atypical.
//
// The numbers here are the ones from the recording that exposed it: 6.0 s
// segments reported as 4.1 s, so every time shown was a third short.
#include "../src/core/model.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

static Manifest with_durations(const std::vector<double>& ds) {
    Manifest m;
    uint64_t seq = 0;
    for (double d : ds) {
        ManifestSegment s;
        s.seq = seq++;
        s.duration_s = d;
        m.push(s, 1000);
    }
    return m;
}

int main() {
    std::printf("== 1. a finished recording's short final fragment ==\n");
    {
        // 6.0 s segments, ended mid-fragment at 4.1 s. Taking the last one
        // put the entire time base 32%% out.
        auto m = with_durations({ 6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 6.0, 4.1 });
        CHECK(near(m.stream_duration_hint(), 6.0),
              "the short final fragment does not set the time base");

        // What that error did downstream, at the scale it happened at: with a
        // live edge 1751 segments ahead of the head, the operator was told
        // 7179 s behind when the truth was 10506 s — nearly an hour out.
        const double behind_wrong  = 1751 * 4.1;
        const double behind_right  = 1751 * m.stream_duration_hint();
        CHECK(near(behind_right, 10506.0), "behind-live is computed on the real grid");
        CHECK(behind_right - behind_wrong > 3000.0,
              "the old estimate understated it by more than 3000 s");
    }

    std::printf("== 2. it still tracks a genuinely different segment length ==\n");
    {
        auto m = with_durations({ 2.0, 2.0, 2.0, 2.0, 2.0 });
        CHECK(near(m.stream_duration_hint(), 2.0), "a 2 s stream reads as 2 s");
        auto ten = with_durations({ 10.0, 10.0, 10.0 });
        CHECK(near(ten.stream_duration_hint(), 10.0), "a 10 s stream reads as 10 s");
    }

    std::printf("== 3. keyframe jitter must not move it ==\n");
    {
        // Real durations vary slightly with keyframe placement; the estimate
        // should sit in the middle of that rather than on an extreme.
        auto m = with_durations({ 5.9, 6.1, 6.0, 5.95, 6.05 });
        const double h = m.stream_duration_hint();
        CHECK(h >= 5.9 && h <= 6.1, "the estimate stays within the jitter band");
    }

    std::printf("== 4. degenerate lists ==\n");
    {
        Manifest empty;
        CHECK(near(empty.stream_duration_hint(), 0.0),
              "no segments means no estimate, not a guess");

        // One full segment and one short one is the smallest case that used to
        // be wrong, and an average would still be wrong here.
        auto two = with_durations({ 6.0, 4.1 });
        CHECK(near(two.stream_duration_hint(), 6.0),
              "one full and one short segment reads as the full one");

        // Durations of zero are unset, not real, and must be ignored rather
        // than dragging the estimate towards nothing.
        auto zeros = with_durations({ 0.0, 0.0, 6.0 });
        CHECK(near(zeros.stream_duration_hint(), 6.0),
              "unset durations are ignored");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL TIMEBASE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
