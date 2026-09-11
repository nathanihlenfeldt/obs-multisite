// SPDX-License-Identifier: GPL-3.0-or-later
// test_audio_queue.cpp — the buffer between a decoded frame and a 1 ms calendar.
//
// This exists because of a real install: an AES67 card whose buffer is eight
// one-millisecond slots, played a twenty-one-millisecond decoded frame, and
// reported about thirty under-runs a second. Something has to hold the
// difference, and the arithmetic that does it — a ring index that wraps, a
// partial read at the end of the buffer — is exactly the kind of thing that is
// silently wrong: an off-by-one loses a millisecond every time the buffer turns
// over, which on a 60 ms buffer is a click every 60 ms, and near-impossible to
// trace back from a log.
//
// None of that needs a sound card, and the class holds no lock of its own, so
// the whole thing is checkable on any machine. That matters: `alsa_output.cpp`
// compiles only where ALSA exists, so a test that lived next to the threads
// would not have run on the laptop where this was written.
#include "audio_queue.h"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// A pattern that makes a wrong byte obvious: byte i is (i * 7 + 3) & 0xff.
static std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)((i * 7 + 3) & 0xff);
    return v;
}

int main() {
    // ── In, out, in the same order ───────────────────────────────────────────
    std::printf("== what goes in comes out, in order ==\n");
    {
        ByteQueue q;
        q.reset(64);
        const std::vector<uint8_t> in = pattern(50);
        CHECK(q.push(in.data(), in.size()) == 50, "a push that fits is taken whole");
        CHECK(q.size() == 50, "size reflects what is held");

        std::vector<uint8_t> out(50);
        CHECK(q.pop(out.data(), 50) == 50, "a pop with enough held returns all of it");
        CHECK(out == in, "every byte is unchanged and in order");
        CHECK(q.empty(), "queue is empty afterwards");
    }

    // ── The partial pop the feeder actually makes ────────────────────────────
    // The feeder takes one 1 ms slot at a time out of a much larger push. This
    // is the read that runs off the end of the backing buffer and wraps.
    std::printf("== reading a slot at a time out of a larger push ==\n");
    {
        ByteQueue q;
        q.reset(96);
        const std::vector<uint8_t> in = pattern(80);   // 10 slots of 8 bytes
        CHECK(q.push(in.data(), in.size()) == 80, "80 bytes accepted");
        std::vector<uint8_t> slot(8), got;
        for (int i = 0; i < 10; ++i) {
            CHECK(q.pop(slot.data(), slot.size()) == 8, "each 8-byte slot is full");
            got.insert(got.end(), slot.begin(), slot.end());
        }
        CHECK(got == in, "ten slices reassemble into the original");
        CHECK(q.empty(), "nothing left over");
    }

    // ── Wraparound, which is where the off-by-one lives ──────────────────────
    // Fill, drain, fill again so head and tail both cross the end of the buffer,
    // and check the data is still exact. A ring whose modulo is wrong passes the
    // one-shot test above and fails here.
    std::printf("== head and tail wrap past the end without losing a byte ==\n");
    {
        ByteQueue q;
        q.reset(16);
        for (int round = 0; round < 20; ++round) {
            const std::vector<uint8_t> in = pattern(16);
            CHECK(q.push(in.data(), in.size()) == 16, "a full buffer's worth fits");
            std::vector<uint8_t> out(16);
            CHECK(q.pop(out.data(), 16) == 16, "and comes back whole");
            CHECK(out == in, "byte-for-byte, on every turn of the ring");
        }
    }

    // A push that does not start at offset zero, so push's own wrap is exercised
    // rather than only pop's.
    std::printf("== push wraps too ==\n");
    {
        ByteQueue q;
        q.reset(10);
        const std::vector<uint8_t> a = pattern(7);
        q.push(a.data(), a.size());          // 7 in, tail at 7
        std::vector<uint8_t> sink(3);
        q.pop(sink.data(), 3);               // 4 left, head at 3
        const std::vector<uint8_t> b = pattern(6);
        // Only 6 free; the write crosses index 9 -> 0.
        CHECK(q.push(b.data(), b.size()) == 6, "the push that crosses the end is taken");
        std::vector<uint8_t> out(10);
        CHECK(q.pop(out.data(), 10) == 10, "all ten are there");
        // Expect: the last four bytes of `a`, then all six of `b`.
        std::vector<uint8_t> want(a.end() - 4, a.end());
        want.insert(want.end(), b.begin(), b.end());
        CHECK(out == want, "the split push lands in the right place");
    }

    // ── A push with no room is short, not a stall ────────────────────────────
    // The point of the class is that the thread showing the picture never waits.
    // Overrunning has to be reported, so the feeder can drop the old audio and
    // the log can say the queue fell behind.
    std::printf("== a push with no room is short rather than blocking ==\n");
    {
        ByteQueue q;
        q.reset(10);
        const std::vector<uint8_t> in = pattern(25);
        CHECK(q.push(in.data(), in.size()) == 10, "only what fits is taken");
        CHECK(q.size() == 10, "the queue is full, not overfull");
        CHECK(q.push(in.data(), in.size()) == 0, "a push into a full queue takes nothing");
    }

    // ── clear() keeps the capacity ───────────────────────────────────────────
    // After a seek the held audio belongs to where playback used to be.
    std::printf("== clear empties without resizing ==\n");
    {
        ByteQueue q;
        q.reset(32);
        const std::vector<uint8_t> in = pattern(20);
        q.push(in.data(), in.size());
        q.clear();
        CHECK(q.empty(), "nothing held after clear");
        CHECK(q.capacity() == 32, "capacity is unchanged");
        const std::vector<uint8_t> b = pattern(20);
        CHECK(q.push(b.data(), b.size()) == 20, "and it is usable again");
    }

    // ── discard, used to drop stale audio without copying it ─────────────────
    std::printf("== discard drops the oldest bytes ==\n");
    {
        ByteQueue q;
        q.reset(16);
        const std::vector<uint8_t> in = pattern(12);
        q.push(in.data(), in.size());
        CHECK(q.discard(5) == 5, "five discarded");
        CHECK(q.size() == 7, "seven remain");
        std::vector<uint8_t> out(7);
        CHECK(q.pop(out.data(), 7) == 7, "the rest come out");
        CHECK(std::vector<uint8_t>(in.begin() + 5, in.end()) == out,
              "what remains is the tail of the original");
        CHECK(q.discard(100) == 0, "discarding more than held takes only what is there");
    }

    // ── Degenerate input must not misbehave ──────────────────────────────────
    std::printf("== zero capacity and zero-length operations ==\n");
    {
        ByteQueue q;                 // never reset
        std::vector<uint8_t> out(4);
        CHECK(q.capacity() == 0 && q.empty(), "an unreset queue is empty");
        const uint8_t one = 1;
        CHECK(q.push(&one, 1) == 0, "pushing to a zero-capacity queue takes nothing");
        CHECK(q.pop(out.data(), 4) == 0, "popping from it returns nothing");
        CHECK(q.discard(1) == 0, "discarding from it does nothing");

        q.reset(8);
        CHECK(q.push(nullptr, 0) == 0, "a zero-length push is a no-op");
        CHECK(q.pop(out.data(), 0) == 0, "a zero-length pop is a no-op");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL AUDIO QUEUE TESTS PASSED"
                                      : "SOME AUDIO QUEUE TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
