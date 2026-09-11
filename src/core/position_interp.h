// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// position_interp.h — where the playhead reads BETWEEN state samples.
//
// A dock that polls state twice a second draws a scrub bar that visibly steps,
// which is not what a tool sitting next to a video player should look like. So
// the playhead is extrapolated from the last sample using wall time.
//
// This is its own function, and tested, because the first attempt at it shipped
// a frozen timer and an apparently dead Play button. That version clamped the
// interpolated value against an absolute end-of-recording bound, so one bad
// input pinned the readout to a constant that no amount of correct state could
// shift — while the backend was healthy and the refresh was running the whole
// time. The two properties below are what prevent that recurring, and they are
// the reason this is not written inline at the call site.
//
//  1. Extrapolation is bounded RELATIVE to the sample it came from. If state
//     refreshes stop, the readout settles a fraction of a second past the last
//     real number rather than running away — and, crucially, it can never be
//     pinned to a value derived from anything but that sample.
//  2. The absolute bound only ever caps; it cannot become the answer on its
//     own, because it is applied after the relative clamp rather than instead
//     of it.
//
#include <cstdint>

namespace multisite {

// base_ms       the last authoritative playhead
// base_wall_ms  the wall-clock time that sample was taken
// now_wall_ms   wall-clock time now
// bound_ms      never report past this (end of recording, live edge); 0 = none
// max_extrap_ms never extrapolate further than this beyond base_ms
inline long long interpolate_position(long long base_ms, long long base_wall_ms,
                                      long long now_wall_ms, long long bound_ms,
                                      long long max_extrap_ms) {
    long long elapsed = now_wall_ms - base_wall_ms;
    // A clock that steps backwards (NTP, sleep/wake) must not rewind the
    // playhead: the sample is still the best answer available.
    if (elapsed < 0) elapsed = 0;
    if (elapsed > max_extrap_ms) elapsed = max_extrap_ms;

    long long head = base_ms + elapsed;
    if (bound_ms > 0 && head > bound_ms) head = bound_ms;
    return head;
}

} // namespace multisite
