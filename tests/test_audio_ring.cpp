// SPDX-License-Identifier: GPL-3.0-or-later
// test_audio_ring.cpp — the queue that sits between a decoded frame and the
// card, and the wraparound that makes it a queue rather than a buffer.
//
// This is `audio_ring.h`, which is the fix for the bench box that logged "sound
// has broken up 8060 times": a 21 ms frame written straight at an 8 ms card.
// The ring is a plain circular buffer, and the parts of a circular buffer that
// are wrong when they are wrong are dull and invisible — the index that wraps at
// the wrong end, the copy that forgets it may wrap mid-write, the count that
// drifts by a byte each pass and only fails after an hour. So they are checked
// here rather than on the Pi.
//
// It includes no ALSA and no threads, which is why it runs anywhere. What is
// NOT covered here is the locking and the feeder thread in `alsa_output.cpp` —
// that needs the card, and it is where the risk now lives.
#include "audio_ring.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// Read the whole ring out, oldest first, without disturbing it.
static std::string contents(const AudioRing& r) {
    AudioRing copy = r;
    std::vector<uint8_t> b(copy.used());
    const size_t n = copy.pop(b.data(), b.size());
    return std::string(b.begin(), b.begin() + (long)n);
}

static void push_str(AudioRing& r, const std::string& s) {
    r.push(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

int main() {
    // ── A ring that has not been sized holds nothing and takes nothing ───────
    std::printf("== an unsized ring is inert ==\n");
    {
        AudioRing r;
        const uint8_t b[4] = { 1, 2, 3, 4 };
        uint8_t out[4] = { 0, 0, 0, 0 };
        CHECK(r.capacity() == 0, "capacity is zero before reset");
        CHECK(r.used() == 0 && r.room() == 0, "used and room are zero");
        CHECK(r.push(b, 4) == 0, "push into an unsized ring takes nothing");
        CHECK(r.pop(out, 4) == 0, "pop from an unsized ring returns nothing");
        CHECK(r.drop_oldest(4) == 0, "drop_oldest on an unsized ring is a no-op");
    }

    // ── Push and pop are FIFO ────────────────────────────────────────────────
    std::printf("== a ring is first in, first out ==\n");
    {
        AudioRing r;
        r.reset(8);
        CHECK(r.capacity() == 8 && r.used() == 0 && r.room() == 8,
              "a fresh ring is empty and full of room");
        push_str(r, "abc");
        CHECK(r.used() == 3 && r.room() == 5, "push advances used");
        std::vector<uint8_t> out(3);
        const size_t n = r.pop(out.data(), 3);
        CHECK(n == 3, "pop returns what it took");
        CHECK(std::string(out.begin(), out.end()) == "abc", "and it is the same order");
        CHECK(r.used() == 0, "the ring is empty again");
    }

    // ── The wraparound: the whole point of the type ──────────────────────────
    std::printf("== a write that spans the end of the buffer ==\n");
    {
        // Fill 6 of 8, read them back, then write 4 more. Those 4 land across
        // the end and the start of the underlying buffer — 2 bytes at index 6
        // and 2 at index 0 — so they are only correct if push splits the copy.
        // A push that wrote only to the tail would silently lose half of them.
        AudioRing r;
        r.reset(8);
        push_str(r, "abcdef");
        {
            std::vector<uint8_t> sink(6);
            CHECK(r.pop(sink.data(), 6) == 6, "drained the first six");
        }
        push_str(r, "gh");
        push_str(r, "ij");
        CHECK(r.used() == 4, "the ring holds all four after wrapping");
        CHECK(contents(r) == "ghij",
              "and reads them back in order across the wrap");
    }

    std::printf("== a read that spans the end of the buffer ==\n");
    {
        // Same shape from the reader's side: the oldest bytes are at the end of
        // the buffer and the newest at the start, so a single memcpy from the
        // head would return the wrong tail.
        AudioRing r;
        r.reset(8);
        push_str(r, "abcdef");
        std::vector<uint8_t> sink(6);
        r.pop(sink.data(), 6);
        push_str(r, "ghijkl");
        CHECK(r.used() == 6, "six in");
        std::vector<uint8_t> out(6);
        const size_t n = r.pop(out.data(), 6);
        CHECK(n == 6, "six out");
        CHECK(std::string(out.begin(), out.end()) == "ghijkl",
              "the split read reassembles in order");
    }

    // ── push never drops; that is the caller's decision ──────────────────────
    std::printf("== push takes what fits and says how much ==\n");
    {
        AudioRing r;
        r.reset(4);
        CHECK(r.push(reinterpret_cast<const uint8_t*>("ab"), 2) == 2,
              "two of two taken");
        CHECK(r.push(reinterpret_cast<const uint8_t*>("cdef"), 4) == 2,
              "two of four taken when only two fit");
        CHECK(r.used() == 4 && r.room() == 0, "the ring is now full");
        CHECK(r.push(reinterpret_cast<const uint8_t*>("g"), 1) == 0,
              "a full ring takes nothing at all");
        CHECK(contents(r) == "abcd", "and kept what it already had");
    }

    std::printf("== nothing in, nothing out ==\n");
    {
        AudioRing r;
        r.reset(4);
        uint8_t out[4];
        CHECK(r.pop(out, 4) == 0, "pop from an empty ring reads nothing");
        CHECK(r.push(nullptr, 4) == 0, "a null source is refused");
        CHECK(r.push(reinterpret_cast<const uint8_t*>("a"), 0) == 0,
              "a zero-length push is refused");
        CHECK(r.pop(out, 0) == 0, "a zero-length pop is refused");
    }
    // ── Dropping the oldest, which is what a full queue must do ──────────────
    std::printf("== a full ring drops the OLDEST, keeping the newest ==\n");
    {
        // This is the choice the player makes when the delivery thread falls a
        // long way behind: lose the audio furthest behind the picture rather
        // than block the thread that paints it. The bytes that survive must be
        // the recent ones, not the ones that were already late.
        AudioRing r;
        r.reset(8);
        push_str(r, "abcdefgh");
        CHECK(r.used() == 8, "full");
        CHECK(r.drop_oldest(2) == 2, "two bytes dropped");
        CHECK(r.used() == 6, "and six remain");
        push_str(r, "ij");
        CHECK(r.used() == 8, "back to full");
        CHECK(contents(r) == "cdefghij",
              "the eight bytes kept are the newest, in order");
        CHECK(r.drop_oldest(100) == 8, "dropping more than it holds takes all of it");
        CHECK(r.used() == 0, "and the ring is empty");
        CHECK(r.drop_oldest(1) == 0, "dropping from an empty ring is a no-op");
    }

    // ── reset() and clear() start over ───────────────────────────────────────
    std::printf("== reset and clear both empty it ==\n");
    {
        AudioRing r;
        r.reset(8);
        push_str(r, "abcd");
        r.clear();
        CHECK(r.used() == 0 && r.room() == 8, "clear empties it");
        CHECK(contents(r) == "", "and it reads as empty");
        push_str(r, "abcd");
        r.reset(8);
        CHECK(r.used() == 0 && r.capacity() == 8, "reset empties it");
        push_str(r, "abcdefgh");
        r.reset(4);
        CHECK(r.capacity() == 4, "reset can resize");
        CHECK(r.used() == 0, "and resizing discards what was there");
    }

    // ── The drift that only shows up after a long time ───────────────────────
    std::printf("== a long run keeps its count and its order ==\n");
    {
        // Ten thousand bytes through an eight-byte ring, one at a time. If any
        // index or count were off by one, the wrap happens often enough here
        // that the sequence comes out wrong or `used` drifts — which is the
        // failure that would surface an hour into a service rather than on the
        // bench, so it is worth the loop.
        AudioRing r;
        r.reset(8);
        bool order_ok = true, count_ok = true;
        size_t produced = 0, consumed = 0;
        uint8_t v = 0;
        while (produced < 10000) {
            if (r.room() > 0) {
                const uint8_t byte = (uint8_t)(produced & 0xFF);
                if (r.push(&byte, 1) != 1) count_ok = false;
                ++produced;
            }
            uint8_t got = 0;
            if (r.pop(&got, 1) == 1) {
                if (got != (uint8_t)(consumed & 0xFF)) order_ok = false;
                ++consumed;
            }
            if (r.used() > r.capacity()) count_ok = false;
        }
        while (r.pop(&v, 1) == 1) ++consumed;
        CHECK(order_ok, "every byte came out in the order it went in");
        CHECK(count_ok, "used never exceeded capacity");
        CHECK(produced == consumed, "everything pushed came out");
    }

    // ── The shape the feeder actually uses: bursty in, steady out ────────────
    std::printf("== a bursty producer and a steady consumer do not lose order ==\n");
    {
        // Closer to the real thing: 21 ms frames arriving in bursts, drained a
        // period at a time. The ring must hold a whole frame, and must hand it
        // back cut into equal pieces that reassemble to exactly the input.
        const size_t frame_bytes = 21 * 48 * 2;   // 21 ms of 48 kHz stereo S16
        const size_t period_bytes = frame_bytes / 21;
        AudioRing r;
        r.reset(frame_bytes * 4);

        std::vector<uint8_t> sent, received;
        for (int frame = 0; frame < 50; ++frame) {
            std::vector<uint8_t> f(frame_bytes);
            for (size_t i = 0; i < f.size(); ++i) f[i] = (uint8_t)(i + frame);
            sent.insert(sent.end(), f.begin(), f.end());
            const size_t took = r.push(f.data(), f.size());
            if (took != f.size()) {
                CHECK(false, "a whole frame fit in a queue four frames deep");
                break;
            }
            // Drain exactly one frame's worth, a period at a time, as the
            // feeder would between arrivals.
            size_t left = frame_bytes;
            while (left > 0) {
                std::vector<uint8_t> chunk(period_bytes);
                const size_t n = r.pop(chunk.data(), chunk.size());
                received.insert(received.end(), chunk.begin(), chunk.begin() + (long)n);
                left -= n;
                if (n == 0) break;
            }
        }
        CHECK(sent == received, "the reassembled stream is byte-for-byte the input");
        CHECK(r.used() == 0, "and the queue drained empty");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL AUDIO RING TESTS PASSED"
                                      : "SOME AUDIO RING TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}

