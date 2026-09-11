// SPDX-License-Identifier: GPL-3.0-or-later
// test_pcm_convert.cpp — the decoder's floats to what the card agreed to take.
//
// This exists because of a real install: an AES67 card advertised S32_LE and
// nothing else, the player asked for float and gave up, and the box went quiet
// with
//
//     hw:CARD=Default,DEV=0 will not take floating-point audio: Invalid argument
//
// The conversion that fixes it is arithmetic, and arithmetic is what can be
// quietly wrong — a saturating clamp that wraps, a scale off by one step, a
// channel that lands in the wrong slot. None of that needs a sound card to
// check, so none of it is checked with one. It runs on any machine, which is
// the point: `alsa_output.cpp` is compiled only where ALSA exists, so a test
// that needed the hardware would not have run on the laptop where this was
// written.
#include "pcm_convert.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// Reading the output the way a driver would: little-endian bytes off the wire.
static int16_t s16_at(const std::vector<uint8_t>& b, size_t i) {
    int16_t v = 0; std::memcpy(&v, b.data() + i * 2, 2); return v;
}
static int32_t s32_at(const std::vector<uint8_t>& b, size_t i) {
    int32_t v = 0; std::memcpy(&v, b.data() + i * 4, 4); return v;
}
static float f32_at(const std::vector<uint8_t>& b, size_t i) {
    float v = 0.0f; std::memcpy(&v, b.data() + i * 4, 4); return v;
}

int main() {
    std::vector<uint8_t> out;

    // ── Float, the path that must stay bit-exact ─────────────────────────────
    // HDMI takes float, and most installs are HDMI. If this path ever started
    // rounding, it would be a change in the audio for no reason at all.
    std::printf("== Float32 passes through untouched ==\n");
    {
        const float in[] = { 0.0f, 0.25f, -0.5f, 1.0f, -1.0f, 0.333333f };
        pcm_convert(in, 3, 2, 2, PcmOutFormat::Float32, out);
        CHECK(out.size() == 3 * 2 * 4, "size is frames x channels x 4 bytes");
        bool same = true;
        for (size_t i = 0; i < 6; ++i)
            if (f32_at(out, i) != in[i]) same = false;
        CHECK(same, "every sample is bit-identical to the input");
    }

    // ── The values a clamp gets wrong ────────────────────────────────────────
    std::printf("== S16 scaling, full scale and saturation ==\n");
    {
        const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f };
        pcm_convert(in, 5, 1, 1, PcmOutFormat::S16, out);
        CHECK(out.size() == 5 * 2, "size is frames x 2 bytes");
        CHECK(s16_at(out, 0) == 0,      "0.0 -> 0");
        CHECK(s16_at(out, 1) == 32767,  "+1.0 -> 32767 (full scale, not 32768)");
        CHECK(s16_at(out, 2) == -32767, "-1.0 -> -32767 (symmetric, not -32768)");
        CHECK(s16_at(out, 3) == 16384,  "+0.5 -> 16384 (rounds to nearest)");
        CHECK(s16_at(out, 4) == -16384, "-0.5 -> -16384");
    }

    std::printf("== out-of-range saturates rather than wrapping ==\n");
    {
        // The failure this guards against is the loud one: a decoder that
        // overshoots hands back 1.5, and a wrapping cast turns "+loud" into
        // full-scale negative — an audible crack, not a quiet clip. ALSA's own
        // plug layer saturates, so `hw:` and `plughw:` must sound the same.
        const float in[] = { 2.0f, 100.0f, -2.0f, -100.0f, 1.5f, -1.5f };
        pcm_convert(in, 6, 1, 1, PcmOutFormat::S16, out);
        CHECK(s16_at(out, 0) == 32767,  "+2.0 saturates to +32767");
        CHECK(s16_at(out, 1) == 32767,  "+100 saturates to +32767");
        CHECK(s16_at(out, 2) == -32767, "-2.0 saturates to -32767");
        CHECK(s16_at(out, 3) == -32767, "-100 saturates to -32767");
        CHECK(s16_at(out, 4) == 32767,  "+1.5 saturates to +32767");
        CHECK(s16_at(out, 5) == -32767, "-1.5 saturates to -32767");
        CHECK(s16_at(out, 0) > 0,       "the loud positive sample stayed positive");
    }

    std::printf("== S32 scaling ==\n");
    {
        const float in[] = { 0.0f, 1.0f, -1.0f, 5.0f, -5.0f };
        pcm_convert(in, 5, 1, 1, PcmOutFormat::S32, out);
        CHECK(out.size() == 5 * 4, "size is frames x 4 bytes");
        CHECK(s32_at(out, 0) == 0,           "0.0 -> 0");
        CHECK(s32_at(out, 1) == 2147483647,  "+1.0 -> 2147483647");
        CHECK(s32_at(out, 2) == -2147483647, "-1.0 -> -2147483647");
        CHECK(s32_at(out, 3) == 2147483647,  "+5.0 saturates");
        CHECK(s32_at(out, 4) == -2147483647, "-5.0 saturates");
    }

    // ── A NaN is a decoder fault, not a click ────────────────────────────────
    std::printf("== NaN becomes silence ==\n");
    {
        const float bad = std::nanf("");
        const float in[] = { bad, 0.0f, bad };
        pcm_convert(in, 3, 1, 1, PcmOutFormat::S16, out);
        CHECK(s16_at(out, 0) == 0, "NaN -> 0, not undefined");
        CHECK(s16_at(out, 2) == 0, "NaN later in the buffer too");
        pcm_convert(in, 3, 1, 1, PcmOutFormat::Float32, out);
        CHECK(f32_at(out, 0) == 0.0f, "NaN -> 0.0 in the float path as well");
    }

    // ── Interleaving is frame-major, which is what ALSA means by it ──────────
    std::printf("== interleaving stays frame-major ==\n");
    {
        // Two frames of stereo, all four values distinct, so a swapped loop
        // (channel-major, the classic slip) cannot pass by luck.
        const float in[] = { 0.1f, 0.2f, 0.3f, 0.4f };
        pcm_convert(in, 2, 2, 2, PcmOutFormat::S16, out);
        CHECK(s16_at(out, 0) == pcm_saturate(0.1f, 16), "frame0 ch0");
        CHECK(s16_at(out, 1) == pcm_saturate(0.2f, 16), "frame0 ch1");
        CHECK(s16_at(out, 2) == pcm_saturate(0.3f, 16), "frame1 ch0");
        CHECK(s16_at(out, 3) == pcm_saturate(0.4f, 16), "frame1 ch1");
    }

    // ── Channels the card will not take ──────────────────────────────────────
    std::printf("== extra channels zeroed, surplus dropped ==\n");
    {
        // Mono feed into a stereo device: the second channel is silence, which
        // is what makes a single track audible on both outputs of a card.
        const float mono[] = { 0.5f, -0.5f };
        pcm_convert(mono, 2, 1, 2, PcmOutFormat::S16, out);
        CHECK(s16_at(out, 0) == pcm_saturate(0.5f, 16),  "mono frame0 -> left");
        CHECK(s16_at(out, 1) == 0,                       "frame0 right is silence");
        CHECK(s16_at(out, 2) == pcm_saturate(-0.5f, 16), "mono frame1 -> left");
        CHECK(s16_at(out, 3) == 0,                       "frame1 right is silence");

        // Eight channels into two: the first two are kept, the rest dropped,
        // rather than refusing to make a sound.
        float eight[8] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f };
        pcm_convert(eight, 1, 8, 2, PcmOutFormat::S16, out);
        CHECK(out.size() == 1 * 2 * 2, "the output is two channels wide");
        CHECK(s16_at(out, 0) == pcm_saturate(0.1f, 16), "kept channel 0");
        CHECK(s16_at(out, 1) == pcm_saturate(0.2f, 16), "kept channel 1");

        // ...and eight into eight keeps all of them: the multi-channel case the
        // whole audio design exists to deliver.
        pcm_convert(eight, 1, 8, 8, PcmOutFormat::S32, out);
        CHECK(out.size() == 1 * 8 * 4, "eight channels out is eight wide");
        bool all = true;
        for (int c = 0; c < 8; ++c)
            if (s32_at(out, (size_t)c) !=
                    pcm_saturate((float)(c + 1) / 10.0f, 32)) all = false;
        CHECK(all, "every one of the eight channels landed in its own slot");
    }

    // ── Sample width follows the format, not a guess ─────────────────────────
    std::printf("== byte width matches the format ==\n");
    {
        CHECK(pcm_bytes_per_sample(PcmOutFormat::Float32) == 4, "float is 4 bytes");
        CHECK(pcm_bytes_per_sample(PcmOutFormat::S32) == 4, "S32 is 4 bytes");
        CHECK(pcm_bytes_per_sample(PcmOutFormat::S16) == 2, "S16 is 2 bytes");
        const float in[] = { 0.0f };
        for (PcmOutFormat fmt : { PcmOutFormat::Float32, PcmOutFormat::S32,
                                  PcmOutFormat::S16 }) {
            pcm_convert(in, 1, 1, 1, fmt, out);
            CHECK(out.size() == (size_t)pcm_bytes_per_sample(fmt),
                  "one frame of one channel is exactly one sample wide");
        }
    }

    // ── Degenerate input must not crash or emit junk ─────────────────────────
    std::printf("== empty input produces empty output ==\n");
    {
        const float in[] = { 1.0f, 2.0f };
        pcm_convert(in, 0, 2, 2, PcmOutFormat::S16, out);
        CHECK(out.empty(), "zero frames -> no bytes");
        pcm_convert(in, 1, 0, 2, PcmOutFormat::S16, out);
        CHECK(out.empty(), "zero source channels -> no bytes");
        pcm_convert(in, 1, 2, 0, PcmOutFormat::S16, out);
        CHECK(out.empty(), "zero destination channels -> no bytes");
    }

    // The buffer is reused across frames on the delivery thread, so it must
    // resize rather than depend on being freshly constructed. A shrink left
    // over from a longer frame would otherwise be written out at the old length.
    std::printf("== a reused buffer shrinks as well as grows ==\n");
    {
        const float two[] = { 1.0f, 1.0f };
        pcm_convert(two, 2, 1, 1, PcmOutFormat::S16, out);
        const size_t big = out.size();
        pcm_convert(two, 1, 1, 1, PcmOutFormat::S16, out);
        CHECK(big == 4 && out.size() == 2,
              "a shorter frame leaves a shorter buffer");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PCM CONVERSION TESTS PASSED"
                                      : "SOME PCM CONVERSION TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}