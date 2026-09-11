// SPDX-License-Identifier: GPL-3.0-or-later
// test_digisyn_calendar.cpp — addressing the AES67 card's calendar of slots.
//
// This exists because of a real install: an AES67 card that is not a queue on a
// clock the player controls, but a calendar the daemon reads on its own. The
// player wrote a whole decoded frame at a time and the card, unable to hold one,
// reported about thirty under-runs a second. The fix addresses slots instead.
//
// The addressing is the part worth checking without a card, because it is
// arithmetic copied from the vendor's Dsp.h and a mistake in it puts a
// millisecond of audio into the wrong millisecond — which sounds like a stutter
// or a warble and is very hard to attribute. The vendor's own definitions are
// reproduced as literals here, so if our helper drifts from them the test says
// so. It runs on any machine; the card exists only on the appliance.
#include "digisyn_calendar.h"

#include <cstdint>
#include <cstdio>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// The header the bench box actually has: 48 kHz, 8 channels, 8 slots.
static DigisynHeader bench_header() {
    DigisynHeader h{};
    h.verifyCodeStart = kDigisynVerifyCode;
    h.mapSize = 409600;
    h.baseToNet = 16384;
    h.baseFromNet = 16384 + 196608;
    h.sampleRate = 48000;
    h.chToNet = 8;
    h.chFromNet = 8;
    h.bufMs = 8;
    h.msIndex = 0;
    h.verifyCodeEnd = kDigisynVerifyCode;
    return h;
}

// The vendor's own Dsp_msToFrame, spelled out as literals for comparison.
static uint32_t vendor_frame(uint32_t rate, uint32_t bufMs, uint64_t ms_index) {
    return (uint32_t)((ms_index % bufMs) * (rate / 1000));
}

int main() {
    const DigisynHeader h = bench_header();

    // ── The size of one slot ─────────────────────────────────────────────────
    // 48000/1000 = 48 frames, times 8 channels, times 4 bytes = 1536. This is
    // the number the probe printed on the bench, so it is a cross-check against
    // the device, not just against arithmetic.
    std::printf("== one 1 ms slot, as the bench box reports it ==\n");
    {
        CHECK(digisyn_slot_samples(h) == 48 * 8, "48 frames x 8 channels");
        CHECK(digisyn_slot_samples(h) * sizeof(int32_t) == 1536,
              "1536 bytes per slot, matching the probe");
    }

    // ── The magic number ─────────────────────────────────────────────────────
    // It is the first eight bytes of "Digisyn_vSndCard" read as a little-endian
    // u64. If this is wrong the player would refuse a healthy device, or worse,
    // accept a foreign mapping — so it is checked against the literal bytes.
    std::printf("== the module's verify code ==\n");
    {
        const char* s = "Digisyn_vSndCard";
        uint64_t code = 0;
        for (int i = 0; i < 8; ++i) code |= (uint64_t)(uint8_t)s[i] << (8 * i);
        CHECK(code == kDigisynVerifyCode, "kDigisynVerifyCode is `Digisyn_` in LE");
    }

    // ── Slot addresses against the vendor's own formula ──────────────────────
    // Slot 0 is at the start of the calendar; each slot advances by one
    // millisecond, i.e. slot_samples samples.
    std::printf("== slot offsets follow Dsp_getBuf_phyToNet_ms ==\n");
    {
        const size_t base = h.baseToNet;
        const size_t slot_bytes = 1536;
        for (uint64_t ms = 0; ms < 8; ++ms) {
            const size_t want = (size_t)base +
                (size_t)vendor_frame(h.sampleRate, h.bufMs, ms) *
                    (size_t)h.chToNet * 4u;
            CHECK(digisyn_slot_offset(h, ms) == want, "offset matches the vendor");
            CHECK(digisyn_slot_offset(h, ms) == base + (size_t)ms * slot_bytes,
                  "consecutive slots are one slot apart");
        }
    }

    // ── The wraparound, which is the whole reason this is a calendar ─────────
    // Slot 0 follows slot 7. An off-by-one here addresses last wrap's audio and
    // is heard as a single millisecond repeated out of place.
    std::printf("== the slot index wraps at bufMs ==\n");
    {
        CHECK(digisyn_slot_frame(h, 8)  == 0,        "ms 8 is slot 0 again");
        CHECK(digisyn_slot_frame(h, 15) == 48 * 7,   "ms 15 is the last slot");
        CHECK(digisyn_slot_frame(h, 16) == 0,        "ms 16 wraps to slot 0");
        CHECK(digisyn_slot_offset(h, 8) == digisyn_slot_offset(h, 0),
              "the offset wraps with it");
        CHECK(digisyn_slot_offset(h, 1000000) ==
                  digisyn_slot_offset(h, 1000000 % 8),
              "a large clock value lands on the same slot as its remainder");
    }

    // ── Which slots are still worth writing ──────────────────────────────────
    // The daemon transmits the slot for the millisecond it is in, so the current
    // slot is already committed and only the future can be written. Writing a
    // slot that has passed, or that is a whole calendar away, would put audio
    // where it will not be read until the wrong moment.
    std::printf("== only future slots within the calendar are writable ==\n");
    {
        CHECK(digisyn_slot_is_ahead(h, 100, 100) == true,  "the current ms is writable");
        CHECK(digisyn_slot_is_ahead(h, 100, 103) == true,  "three ahead is writable");
        CHECK(digisyn_slot_is_ahead(h, 100, 107) == true,  "seven ahead is writable");
        CHECK(digisyn_slot_is_ahead(h, 100, 108) == false, "eight ahead has wrapped back");
        CHECK(digisyn_slot_is_ahead(h, 100, 99)  == false, "a past millisecond is not");
    }

    // ── A degenerate header must not divide by zero ──────────────────────────
    std::printf("== a header with no slots is handled ==\n");
    {
        DigisynHeader z = bench_header();
        z.bufMs = 0;
        CHECK(digisyn_slot_frame(z, 5) == 0, "bufMs 0 does not divide by zero");
        CHECK(digisyn_slot_samples(z) == 48 * 8, "slot size is still computable");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL DIGISYN CALENDAR TESTS PASSED"
                                      : "SOME DIGISYN CALENDAR TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
