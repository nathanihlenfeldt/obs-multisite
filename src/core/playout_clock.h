// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// playout_clock.h — when a decoded frame is due at the output.
//
// Shared by both receiving ends (the OBS source and the campus player),
// because getting this wrong is the same failure in both.
//
#include <cstdint>

namespace multisite {

// The wall-clock moment this frame should be handed to the output.
//
// `first` is the anchor: the pts of whichever frame arrived first after the
// playout clock was last re-anchored. Which STREAM that was is a race —
// audio and video are interleaved within a CMAF fragment rather than
// aligned, measured at ~344ms apart in the field, and the logs show the
// anchor landing on audio on one resume and video on the next. So the other
// stream's following frames routinely carry a pts EARLIER than the anchor,
// and `pts - first` is legitimately negative. The playout base carries a
// deliberate cushion to absorb exactly that.
//
// READ THIS BEFORE "FIXING" THE ARITHMETIC. The four call sites this
// replaced all wrote:
//
//     base + (uint64_t)(pts - first)
//
// which looks like an unsigned-wrap bug for negative deltas and is not.
// Unsigned overflow is modular, so the wrap in the cast cancels the wrap in
// the addition and the result is bit-identical to the signed form for every
// value that matters. This was investigated as a suspected cause of lipsync
// drift and cleared: the two forms were compared directly and agree. It is
// NOT the cause of any sync problem, and swapping it will not fix one.
//
// The signed form is kept for two smaller reasons: it says what it means,
// and it clamps rather than wrapping in the one case where the two genuinely
// differ — a frame due before the epoch, which needs a delta more negative
// than the base and so cannot arise while the base is nanoseconds of uptime.
// test_playout_clock pins both the equivalence and that one difference, so
// the next person to find this cast suspicious can read the test instead of
// repeating the investigation.
inline uint64_t playout_due_ns(uint64_t base_ns, int64_t pts_ns,
                               int64_t first_pts_ns) {
    const int64_t due = (int64_t)base_ns + (pts_ns - first_pts_ns);
    return due > 0 ? (uint64_t)due : 0;
}

} // namespace multisite
