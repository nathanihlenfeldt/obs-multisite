// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// pcm_convert.h — turning the decoder's floats into whatever the card takes.
//
// The player used to ask ALSA for interleaved float and treat a refusal as the
// end of the matter. That is right on HDMI, which takes float, and wrong on
// everything else. The AES67 card is the case that found it: the vendor driver
// advertises `hw->formats = SNDRV_PCM_FMTBIT_S32_LE` and nothing else
// (DigiAes67KoLib/Digisyn-vSndCard.c), so `hw:CARD=…` — which has no plugin in
// the way to convert — answered EINVAL and the install went silent with
//
//     hw:CARD=Default,DEV=0 will not take floating-point audio: Invalid argument
//
// The device list in the interface offers `hw:` entries, so simply choosing the
// card from the menu was enough to cause it. `plughw:` hides the same fault
// behind ALSA's plug layer, which is why the installer writes `plughw:` — but
// relying on a plugin to paper over a device we cannot open directly is a thin
// defence, and it is the wrong answer for a box that is meant to work.
//
// So the card is asked for float first (what the decoder produces arrives
// unchanged, which costs nothing), then 32-bit, then 16-bit integers, and the
// conversion happens here when one of the integer formats is what the card
// agreed to.
//
// This header deliberately includes no ALSA: the arithmetic is the part that
// can be wrong, so it is kept where a test can reach it without the hardware.
// `tests/test_pcm_convert.cpp` checks it.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace multisite_player {

// What the card agreed to accept.
enum class PcmOutFormat { Float32, S32, S16 };

inline int pcm_bytes_per_sample(PcmOutFormat f) {
    switch (f) {
        case PcmOutFormat::Float32: return 4;
        case PcmOutFormat::S32:     return 4;
        case PcmOutFormat::S16:     return 2;
    }
    return 4;
}

// A one-line name for the log, so a description can say which one was taken.
inline const char* pcm_format_name(PcmOutFormat f) {
    switch (f) {
        case PcmOutFormat::Float32: return "float";
        case PcmOutFormat::S32:     return "S32";
        case PcmOutFormat::S16:     return "S16";
    }
    return "?";
}

// Float in [-1, 1] to the nearest integer step of a signed `bits`-wide sample.
//
// Saturating, not wrapping. A decoder that overshoots — a clipped master, or a
// bad upmix — hands us values above 1.0, and wrapping turns a loud passage into
// a full-scale crack on the one night it matters. ALSA's own plug layer
// saturates, so doing the same here means `hw:` and `plughw:` sound alike
// instead of merely both working.
//
// Full scale is the largest POSITIVE step (32767, 2147483647) in both
// directions, matching libswresample and the plug layer. A NaN becomes silence:
// it is a decoder fault, and one bad sample should not become a click.
inline int32_t pcm_saturate(float v, int bits) {
    const double scale = (double)(((int64_t)1 << (bits - 1)) - 1);
    double x = (double)v;
    if (!(x == x)) x = 0.0;          // NaN
    if (x >  1.0) x =  1.0;
    if (x < -1.0) x = -1.0;
    // Round half away from zero, over the magnitude. Adding 0.5 and truncating
    // — the obvious way — rounds +0.5 up but -0.5 toward zero, so the two
    // halves of a symmetric waveform land a step apart. That is a DC offset of
    // one LSB on a pure tone: inaudible, but wrong, and exactly the kind of
    // drift that is invisible until something later depends on the symmetry.
    const double a = std::floor(std::fabs(x) * scale + 0.5);
    return (int32_t)(x < 0.0 ? -a : a);
}

// `frames` frames of `src_channels` interleaved floats into `dst`, as
// `dst_channels` of `fmt`.
//
// Channels are padded with silence or dropped from the end, which is the policy
// the float path already used: play what the card will take and say so in the
// log, rather than refusing to make a sound at all.
//
// Samples are written in the host's byte order, which on every target here
// (aarch64, x86_64) is little-endian — the same assumption the float path
// already made by handing ALSA its own floats `SND_PCM_FORMAT_FLOAT_LE`.
inline void pcm_convert(const float* src, uint32_t frames, int src_channels,
                        int dst_channels, PcmOutFormat fmt,
                        std::vector<uint8_t>& dst) {
    const int bps = pcm_bytes_per_sample(fmt);
    if (src_channels <= 0 || dst_channels <= 0 || frames == 0) {
        dst.clear();
        return;
    }
    dst.resize((size_t)frames * (size_t)dst_channels * (size_t)bps);

    const int copy = src_channels < dst_channels ? src_channels : dst_channels;
    size_t o = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        const float* in = src + (size_t)f * (size_t)src_channels;
        for (int c = 0; c < dst_channels; ++c) {
            const float v = (c < copy) ? in[c] : 0.0f;
            switch (fmt) {
                case PcmOutFormat::Float32: {
                    const float t = (v == v) ? v : 0.0f;
                    std::memcpy(&dst[o], &t, 4);
                    o += 4;
                    break;
                }
                case PcmOutFormat::S32: {
                    const int32_t s = pcm_saturate(v, 32);
                    std::memcpy(&dst[o], &s, 4);
                    o += 4;
                    break;
                }
                case PcmOutFormat::S16: {
                    const int32_t s = pcm_saturate(v, 16);
                    const int16_t h = (int16_t)s;
                    std::memcpy(&dst[o], &h, 2);
                    o += 2;
                    break;
                }
            }
        }
    }
}

} // namespace multisite_player
