// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// digisyn_calendar.h — the arithmetic for addressing the AES67 card's calendar.
//
// The vendor's Digisyn_vSndCard is not a sound card in the usual sense. It
// exposes an ALSA PCM device whose hardware pointer is not driven by any DMA:
//
//     .prepare = dummy_pcm_prepare,   // returns 0 — nothing is reset
//     .pointer = dummy_pcm_pointer,   // (sampleRate/1000 * msIndex) % buffer_size
//
// and the position it reports is a function of `msIndex`, a millisecond counter
// the DigiAes67Proc daemon free-runs. The daemon reads a calendar of `bufMs`
// one-millisecond slots and transmits, for the whole of each millisecond, the
// slot for that millisecond — whether or not anybody wrote it.
//
// That is why the player's usual "hand a decoded frame to ALSA and let the ring
// sort it out" approach produced about thirty under-runs a second (~21 ms frame
// into an 8 ms window, one per frame): the window cannot hold a frame, and the
// pointer moves on regardless. The card is not behind; it is on a clock.
//
// So the audio is written where the daemon will look for it. `Dsp_getBuf_phyToNet_ms`
// (vendor Dsp.h) is exactly this sum — a base offset plus the frame index of the
// millisecond, times the channel count — and it is reproduced here, without any
// device knowledge, so the index arithmetic can be checked against the vendor's
// definition on a machine that has no card in it.
//
// Nothing here opens a device. `tests/test_digisyn_calendar.cpp` checks it.
#include <cstddef>
#include <cstdint>

namespace multisite_player {

// The layout the module publishes in the first page of its mapping. Field order
// and widths are the vendor's `struct Dsp` in DigiAes67KoLib/Dsp.h; it is
// repeated rather than included because the vendor header is not shipped in
// this source tree and a build here must not depend on the card being present.
struct DigisynHeader {
    uint64_t verifyCodeStart;
    uint32_t mapSize;
    uint32_t baseToNet;   // offset from the mapping to the playback calendar
    uint32_t baseFromNet; // offset from the mapping to the capture calendar
    uint32_t sampleRate;
    uint32_t chToNet;
    uint32_t chFromNet;
    uint32_t bufMs;
    volatile uint64_t msIndex;
    uint64_t verifyCodeEnd;
};

// `*(uint64_t*)"Digisyn_"`, the module's own stamp (the first eight bytes of
// DSP_DEVICE_NAME). It writes this into both ends of the header and refuses
// nothing if it is absent, so the reader has to be the one to check. Without
// the check a mapping of a different device would be read as a calendar and
// scribbled on. The value is the bytes little-endian on every target here.
constexpr uint64_t kDigisynVerifyCode = 0x5f6e797369676944ULL;

// The frame index (not byte index) of the start of a given millisecond's slot.
// Wraps through the calendar: slot 0 follows slot bufMs-1.
inline uint32_t digisyn_slot_frame(const DigisynHeader& h, uint64_t ms_index) {
    if (h.bufMs == 0) return 0;
    return (uint32_t)((ms_index % h.bufMs) * (h.sampleRate / 1000));
}

// Samples per one-millisecond slot, across all channels.
inline uint32_t digisyn_slot_samples(const DigisynHeader& h) {
    return (h.sampleRate / 1000) * h.chToNet;
}

// Byte offset, from the start of the mapping, of a given millisecond's slot.
// This is the vendor's Dsp_getBuf_phyToNet_ms expressed as an offset, which is
// what a caller that mmap'd the device actually has to index with.
inline size_t digisyn_slot_offset(const DigisynHeader& h, uint64_t ms_index) {
    return (size_t)h.baseToNet +
           (size_t)digisyn_slot_frame(h, ms_index) * (size_t)h.chToNet *
               sizeof(int32_t);
}

// How far ahead of the daemon's clock to write. The daemon transmits the slot
// for the millisecond it is currently in, so writing the *current* slot is
// already too late — the receive end may have passed it. A couple of
// milliseconds of lead means a slow write lands before its slot is read; the
// cost is that many milliseconds of latency, which on a synchronised service is
// inaudible and on a lip-sync-critical one is worth measuring.
constexpr uint64_t kDigisynLeadMs = 3;

// Whether a millisecond's slot is still in the future, i.e. worth writing. A
// slot that has already been transmitted (or been overwritten by a wraparound)
// must not be written: doing so would put this millisecond's audio into a slot
// the daemon will not read until the whole calendar has gone round, which is
// up to bufMs milliseconds of a sample repeated at the wrong time.
inline bool digisyn_slot_is_ahead(const DigisynHeader& h, uint64_t ms_index,
                                  uint64_t target_ms) {
    if (target_ms < ms_index) return false;
    return target_ms - ms_index < h.bufMs;
}

} // namespace multisite_player
