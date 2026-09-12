// SPDX-License-Identifier: GPL-3.0-or-later
// test_audio_plan.cpp — how wide the sound card is opened, and why not two.
//
// This exists because of a real install. A box with the network audio output
// switched on opened its card with the two-channel fallback before any feed had
// arrived; the AES67 card took that, and "follow the feed" then latched a
// two-channel stream against an eight-channel feed. Every listener heard
// channels 0 and 1 of eight, for the life of the process, and nothing anywhere
// said so — the card had opened successfully and the stream was published.
//
// The decision is one function whose whole point is that it is checkable without
// a card: audio_plan.h touches no ALSA, no config file and no clock, so this runs
// on the laptop the fault was worked out on, where alsa_output.cpp is not
// compiled at all.
#include "audio_plan.h"

#include <cstdio>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// A box as the settings page would leave it. Everything at its default, then
// each case changes only what it is about.
static Config box() { return Config{}; }

int main() {
    // ── On the network, the published width is the answer ────────────────────
    // The daemon's SDP names a fixed width that consoles configure against, so
    // the card is opened at that width and not at whatever a feed happens to
    // carry. This is the case the fault was in.
    std::printf("== on the network, the width is the width being published ==\n");
    {
        Config c = box();
        c.aes67_manage = true;
        c.aes67_channels = 8;
        CHECK(desired_audio_channels(c, 0) == 8,
              "eight published, eight opened — before any frame has arrived");
        CHECK(desired_audio_channels(c, 8) == 8, "and the same once it has");
        CHECK(desired_audio_channels(c, 6) == 8,
              "a six-channel feed does not shrink a published eight");
        // The regression, stated as a number: with nothing known yet the answer
        // must never be the two-channel fallback that latched.
        CHECK(desired_audio_channels(c, 0) != 2,
              "and never the two-channel fallback that caused the fault");
    }

    // ── A published width of zero is not zero channels ───────────────────────
    // aes67_channels_to_map is where "unset" becomes eight, and it is the same
    // call the source body is built from, so the stream and the card cannot
    // disagree about how many channels there are.
    std::printf("== the published width is clamped, by the same call as the SDP ==\n");
    {
        Config c = box();
        c.aes67_manage = true;

        c.aes67_channels = 0;
        CHECK(desired_audio_channels(c, 0) == kAes67DefaultChannels,
              "unset publishes the default, and that is what the card is opened at");

        c.aes67_channels = kAes67MaxChannels + 40;
        CHECK(desired_audio_channels(c, 0) == kAes67MaxChannels,
              "an over-wide setting is clamped, not passed to the card");

        c.aes67_channels = 2;
        CHECK(desired_audio_channels(c, 8) == 2,
              "a deliberately narrow stream stays narrow against a wide feed");
    }

    // ── A width set by hand is an instruction ───────────────────────────────
    std::printf("== a width set by hand is obeyed ==\n");
    {
        Config c = box();
        c.audio_channels = 6;
        CHECK(desired_audio_channels(c, 0) == 6,
              "six asked for is six opened, before any frame has arrived");
        CHECK(desired_audio_channels(c, 2) == 6,
              "and a feed that disagrees does not override it");
    }

    // ── Otherwise follow the feed ───────────────────────────────────────────
    std::printf("== otherwise, follow the feed ==\n");
    {
        Config c = box();
        CHECK(c.audio_channels == 0, "a plain box has no width set");
        CHECK(desired_audio_channels(c, 8) == 8, "eight in the feed is eight on the card");
        CHECK(desired_audio_channels(c, 1) == 1, "mono is honoured as one");
    }

    // ── And with nothing to go on, say so ───────────────────────────────────
    // This is the single most important line in the file. Zero means "not yet",
    // and the caller waits: the first decoded frame arrives within a second of
    // anything playing, so waiting costs nothing, while a guess is what put the
    // wrong eight channels in the wrong places.
    std::printf("== with nothing to go on, the answer is \"not yet\" ==\n");
    {
        Config c = box();
        const int got = desired_audio_channels(c, 0);
        CHECK(got == 0, "no width set and no feed seen is 0, meaning wait");
        CHECK(got != 2, "and specifically not a guessed two");
    }

    // ── Precedence, in the order the comments claim ─────────────────────────
    // The three sources can all be present at once, and the code has to agree
    // with the note at the top of the header about which one wins.
    std::printf("== precedence: network, then hand, then feed ==\n");
    {
        Config c = box();
        c.aes67_manage = true;
        c.aes67_channels = 8;
        c.audio_channels = 2;   // left over from before the network was switched on
        CHECK(desired_audio_channels(c, 4) == 8,
              "the network wins over a width set by hand and over the feed");
    }
    {
        Config c = box();
        c.audio_channels = 4;
        CHECK(desired_audio_channels(c, 8) == 4,
              "a width set by hand wins over the feed");
    }

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall checks passed\n");
    return 0;
}
