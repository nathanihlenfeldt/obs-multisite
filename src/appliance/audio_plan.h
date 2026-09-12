// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// audio_plan.h — how wide the sound card is opened, decided in one place.
//
// This is one small function, and it is a header with a test of its own because
// getting it wrong is the sort of fault nobody sees in a log: the card opens, it
// reports success, the stream is published, and the sound is simply in the wrong
// places. It happened. A box with the network output switched on opened its card
// with the two-channel fallback before any feed had arrived, the AES67 card took
// it, and "follow the feed" then latched that two-channel stream against an
// eight-channel feed. Every listener heard channels 0 and 1 of eight, for the
// life of the process, and nothing anywhere said so.
//
// The rule, restated so it can be checked against the code:
//
//   • With the network output switched on, the width is the width being
//     published. The daemon's SDP names it, consoles configure against it, and
//     it is fixed on purpose (aes67.h says why) — so the card is opened at that
//     width whether anything is playing or not, which is also what keeps the
//     stream up between services.
//   • Otherwise, a width set in the interface is obeyed.
//   • Otherwise, follow the feed.
//   • And if none of those gives a number — a plain box with no width set and no
//     frame seen yet — the answer is 0, meaning "not yet", NOT a guessed two.
//     A guess is what caused the fault above; waiting costs nothing, because
//     the first decoded frame arrives within a second of anything playing.
//
// Nothing here touches ALSA, a config file or a clock, so tests/test_audio_plan.cpp
// checks it on a laptop with no card in it.
#include "config.h"
#include "aes67.h"   // aes67_channels_to_map: the published width, clamped

namespace multisite_player {

// The number of channels the sound card should be opened with right now, or 0
// when there is nothing to open for yet. See the note above for why 0 is not 2.
inline int desired_audio_channels(const Config& cfg, int feed_channels) {
    // On the network, the width is what is being published — the same number
    // the source body is built from, taken from the same function, so the
    // stream and the card cannot disagree about how many channels there are.
    if (cfg.aes67_manage) return aes67_channels_to_map(cfg.aes67_channels);

    // A width set by hand is an instruction, not a hint.
    if (cfg.audio_channels > 0) return cfg.audio_channels;

    // Otherwise follow the feed — but only once a feed has said how wide it is.
    return feed_channels > 0 ? feed_channels : 0;
}

} // namespace multisite_player
