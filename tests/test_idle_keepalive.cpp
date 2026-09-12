// SPDX-License-Identifier: GPL-3.0-or-later
// test_idle_keepalive.cpp — the cushion, and the buffer that holds it.
//
// This exists because those two numbers were computed in two different places
// and came apart, and what came out of the gap was not quiet. The keep-alive
// thread fed the sound card uninitialised heap — read as float, so full-scale
// digital noise at 0 dBFS — out onto the AES67 stream every time the box went
// idle with the sound switched on.
//
// The check that matters is not that a particular number is right. It is that
// the buffer is never shorter than the largest write made from it, for ANY card
// geometry a driver might grant. The geometry that found the fault is in the
// table below: a one millisecond period, 128 periods to a buffer, which is what
// the RAVENNA card reports.
#include "../src/appliance/idle_keepalive.h"

#include <cstdio>
#include <cstdint>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

namespace {

// A card as a driver might describe it. `frame_bytes` is channels times the
// bytes per sample ALSA agreed to: 8 channels of float, the case on this box,
// is 32.
struct Card {
    const char* name;
    uint32_t    rate;
    uint64_t    period;        // frames in a period
    uint64_t    buffer;        // frames in the buffer
    size_t      frame_bytes;
};

const Card kCards[] = {
    // The card that found it: 1 ms periods, 128 of them, eight channels of
    // float. 20 ms of cushion against a buffer sized for one period was a
    // twenty-fold read past the end.
    { "RAVENNA 1 ms period x128", 48000,   48,   6144, 32 },
    // An ordinary HDMI card, where the two happen to agree because the period
    // is the same size as the cushion.
    { "HDMI 40 ms period",        48000, 1920,  24000, 32 },
    { "stereo 10 ms period",      48000,  480,   4800,  8 },
    // A tiny buffer: fewer than four periods, so the two clamps cannot both be
    // satisfied and the period has to win.
    { "three periods total",      48000,   48,    144, 32 },
    { "one period total",         48000,   48,     48, 32 },
    // Rates other than 48 kHz, and a card that granted a huge buffer.
    { "44.1 kHz",                 44100,  441,  22050,  8 },
    { "96 kHz 1 ms period",       96000,   96,  12288, 32 },
    { "192 kHz",                 192000,  192,  24576, 32 },
    // A very small period relative to the rate: the worst ratio of cushion to
    // period there is, and the one most likely to expose a sizing mistake.
    { "1 sample period",          48000,    1,   4096, 32 },
    { "16 sample period",         48000,   16,   2048, 32 },
};

// Every one of these has to hold: the buffer must be long enough for the
// cushion, because the cushion is the largest write the top-up makes.
void test_buffer_holds_the_cushion() {
    std::printf("== the buffer is never shorter than a write from it ==\n");
    for (const Card& c : kCards) {
        const uint64_t cushion =
            idle_cushion_frames(c.rate, c.period, c.buffer);
        const size_t bytes = silence_buffer_bytes(c.frame_bytes, c.rate,
                                                  c.period, c.buffer);
        // This is the exact assertion the old code failed: the buffer was sized
        // from the period while the write was a cushion.
        CHECK(bytes / c.frame_bytes >= cushion,
              "buffer holds a whole cushion");
        CHECK(bytes >= c.frame_bytes * (size_t)cushion,
              "and a full top-up fits in it");
    }
}

void test_the_card_that_found_it() {
    std::printf("== the geometry that found the fault ==\n");
    // 48 kHz, 1 ms period, 128 periods: the RAVENNA card's own numbers, from
    // the log line "128 ms buffer, 1 ms of it per period, 128 periods".
    const Card& c = kCards[0];
    const uint64_t cushion = idle_cushion_frames(c.rate, c.period, c.buffer);
    const size_t bytes = silence_buffer_bytes(c.frame_bytes, c.rate,
                                              c.period, c.buffer);

    CHECK(cushion == 960, "the cushion is 20 ms — 960 frames at 48 kHz");
    CHECK(bytes == 960 * 32, "and the buffer is 960 frames of 8-channel float");
    // The old sizing was one period: 48 frames, 1536 bytes. Stated here so the
    // size of the mistake is on the record rather than only in a commit message.
    CHECK(bytes > c.frame_bytes * c.period * 10,
          "which is more than ten times what sizing it from a period gave");
    CHECK(bytes / c.frame_bytes >= cushion,
          "so a full top-up cannot read past it");
}

void test_clamps() {
    std::printf("== the cushion never lets the card run dry, or crowd it out ==\n");
    for (const Card& c : kCards) {
        const uint64_t cushion =
            idle_cushion_frames(c.rate, c.period, c.buffer);
        // Never less than a period: under it the device under-runs every cycle
        // and stops producing samples, which takes the AES67 stream off air.
        CHECK(cushion >= c.period, "at least one period");
        // Never more than a quarter of the buffer, so it cannot crowd out the
        // device's own headroom — unless the buffer is too small for both
        // clamps, in which case a period wins and this is allowed to break.
        CHECK(cushion <= c.buffer / 4 || cushion == c.period,
              "no more than a quarter of the buffer, or a period if that wins");
        CHECK(cushion > 0, "and never zero on a usable card");
    }
}

void test_degenerate_cards() {
    std::printf("== a card that says nothing gets nothing ==\n");
    // A driver that reports no period, or no buffer, cannot be topped up. The
    // answer has to be zero — not a guessed cushion, and above all not a write
    // into a buffer that was never sized.
    CHECK(idle_cushion_frames(48000, 0, 0) == 0, "no period, no cushion");
    CHECK(idle_cushion_frames(48000, 0, 6144) == 0, "no period, no cushion");
    CHECK(idle_cushion_frames(48000, 48, 0) == 0, "no buffer, no cushion");
    CHECK(idle_cushion_frames(0, 48, 6144) == 0, "no rate, no cushion");
    CHECK(silence_buffer_bytes(32, 48000, 0, 0) == 0, "and no buffer of zeros");
    CHECK(silence_buffer_bytes(0, 48000, 48, 6144) == 0,
          "a card with no frame size gets no buffer");
}

void test_sleep_interval() {
    std::printf("== the interval between top-ups ==\n");
    // A quarter of the cushion, so four can be missed before the card runs dry.
    // At 48 kHz a 960-frame cushion is 20 ms, so 5000 us.
    CHECK(idle_sleep_us(960, 48000) == 5000, "a quarter of a 20 ms cushion");
    // Floored, so a very high rate does not become a spin.
    CHECK(idle_sleep_us(1, 768000) == 1000, "floored at a millisecond");
    // Ceiled, so a very low rate cannot let the card run dry between checks.
    CHECK(idle_sleep_us(48000, 8000) == 20000, "ceiled at twenty milliseconds");
    CHECK(idle_sleep_us(0, 48000) == 1000, "a cushion of nothing still sleeps");
    CHECK(idle_sleep_us(960, 0) == 1000, "and so does a rate of nothing");
}

void test_no_overflow() {
    std::printf("== arithmetic that cannot wrap ==\n");
    // A card reporting an enormous buffer — or a driver with a bug — must not
    // produce a cushion that wraps around into something tiny, because a tiny
    // cushion is safe but a wrapped SIZE is not. Here the 20 ms asked for is far
    // under a quarter of the buffer, so no clamp applies and the answer is just
    // the 20 ms.
    const uint64_t huge = 1ULL << 40;
    const uint64_t cushion = idle_cushion_frames(48000, 48, huge);
    CHECK(cushion == 960, "a huge buffer still asks for only 20 ms");
    CHECK(cushion >= 48, "and at least a period");
    // The byte count still covers the cushion it will be written from, and the
    // multiplication does not overflow: 960 frames of 32 bytes is small.
    const size_t bytes = silence_buffer_bytes(32, 48000, 48, huge);
    CHECK(bytes == 960 * 32, "and the buffer covers all of it");
    CHECK(bytes / 32 >= cushion, "so a top-up cannot read past the end");

    // The other extreme: a buffer so large that a quarter of it is still more
    // than 20 ms, which is where the quarter-of-the-buffer clamp actually bites.
    // 2^20 frames at 48 kHz is about 22 seconds, so a quarter of it is 5.5 s —
    // far more than the 960 frames wanted, so the 20 ms wins here too. The clamp
    // only ever reduces the cushion, so the failure to guard against is the
    // multiplication, and this checks it stays exact.
    const uint64_t big = 1ULL << 20;
    CHECK(idle_cushion_frames(48000, 48, big) == 960,
          "and a merely large one is still 20 ms");
    CHECK(silence_buffer_bytes(32, 48000, 48, big) == 960 * 32,
          "with the buffer to match");
}

} // namespace

int main() {
    test_buffer_holds_the_cushion();
    test_the_card_that_found_it();
    test_clamps();
    test_degenerate_cards();
    test_sleep_interval();
    test_no_overflow();

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall checks passed\n");
    return 0;
}
