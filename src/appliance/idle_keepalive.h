// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// idle_keepalive.h — how much silence to keep in the card, and how big the
// buffer holding it has to be.
//
// Two numbers, and they have to agree. The first is the cushion: how much audio
// the keep-alive thread tries to leave queued in the card while nothing is
// playing. The second is the size of the buffer of zeros it writes from. They
// were computed in two different places, and on a card whose period is one
// millisecond they came apart: the cushion was twenty milliseconds of frames
// while the buffer held one period of them, so the top-up read roughly twenty
// times past the end of its own buffer.
//
// What that sounds like is worth writing down, because it is not what anybody
// would guess. The bytes past the end of the buffer are heap — whatever the
// allocator last put there — and ALSA is being asked to play them as float
// samples. A byte pattern read as IEEE-754 is almost never small: a large
// exponent is full-scale, so the result is not quiet noise but *loud* noise,
// digital hash at 0 dBFS, on the stream every receiver downstream is subscribed
// to. Silence that is actually full scale is the worst possible failure of a
// function whose whole job is to be inaudible, and it is why the size and the
// cushion now come from one place and are checked by a test rather than by
// whoever next changes one of them.
//
// Both live here, in a header with no ALSA in it, so the arithmetic can be
// exercised on a machine with no sound card — including the card geometry that
// found the fault. `tests/test_idle_keepalive.cpp` checks it.
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace multisite_player {

// The target cushion, in frames: how much audio to leave in the card while
// nothing is playing, given the rate the card is running at and what it granted
// in the way of a period and a buffer.
//
// Twenty milliseconds is the cushion. Enough that a wake-up missed by a
// scheduler hiccup does not reach the end of it, and short enough that at most
// that much silence sits in front of the sound when playback starts again.
//
// Clamped at both ends, and both clamps matter:
//
//   • Never less than a period. A period is the least the device can be given,
//     and is also the amount that starts a stopped stream (the start threshold
//     set when the card is opened). Under it the device under-runs on every
//     cycle — which is not merely silence: a card that has stopped producing
//     samples gives the AES67 daemon nothing to publish, and the receivers
//     declare the source offline.
//   • Never more than a quarter of the buffer, so this cannot crowd out the
//     device's own headroom.
//
// The order of the two clamps is deliberate and is not a mistake to be tidied
// away. On a card whose buffer is smaller than four periods there is no answer
// that satisfies both, and a period is the one that cannot be given up: a
// top-up that is too large wastes headroom, whereas a top-up smaller than a
// period does not play at all.
inline uint64_t idle_cushion_frames(uint32_t rate, uint64_t period_frames,
                                    uint64_t buffer_frames) {
    if (rate == 0 || period_frames == 0 || buffer_frames == 0) return 0;

    uint64_t want = (uint64_t)rate / 50;                 // 20 ms
    if (want < period_frames) want = period_frames;
    if (want > buffer_frames / 4) want = buffer_frames / 4;
    if (want < period_frames) want = period_frames;      // a buffer under 4 periods
    return want;
}

// The size of the buffer of zeros, in bytes, that the cushion has to be written
// from.
//
// This is the number that was wrong. The buffer must hold the largest single
// write the keep-alive can ever make, and that is the cushion — not a period.
// Sizing it for a period while writing a cushion from it is an out-of-bounds
// read that produces full-scale noise rather than silence.
inline size_t silence_buffer_bytes(size_t frame_bytes, uint32_t rate,
                                   uint64_t period_frames,
                                   uint64_t buffer_frames) {
    if (frame_bytes == 0) return 0;
    const uint64_t frames =
        idle_cushion_frames(rate, period_frames, buffer_frames);
    if (frames == 0) return 0;
    return frame_bytes * (size_t)frames;
}

// Between checks, in microseconds: a quarter of the cushion, so four of them can
// be missed before the card runs dry.
//
// Microseconds rather than milliseconds, because on a card with a one
// millisecond period a sleep rounded up to the next millisecond is already
// longer than the period itself. The floor keeps a very high rate from becoming
// a spin; the ceiling keeps a very low one from letting the card run dry.
inline long long idle_sleep_us(uint64_t cushion_frames, uint32_t rate) {
    if (rate == 0 || cushion_frames == 0) return 1000;
    long long us = (long long)(cushion_frames * 1000000ULL / rate) / 4;
    if (us < 1000)  us = 1000;
    if (us > 20000) us = 20000;
    return us;
}

} // namespace multisite_player
