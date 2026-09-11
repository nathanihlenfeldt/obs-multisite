// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// audio_ring.h — the queue between the decoder's frames and the card's buffer.
//
// The two sizes do not match and cannot be made to. A decoded frame is one AAC
// frame — 21 ms at 48 kHz — while the AES67 card's driver fixes its own buffer
// at a millisecond per period and only as many periods as its `bufMs`, eight by
// default (`DigiAes67KoLib/Digisyn-vSndCard.c`, period_bytes_min/max and
// periods_min/max). Writing a 21 ms frame straight at an 8 ms card therefore
// blocks for the difference — and it blocks the thread that also presents the
// picture, so the card empties every time that thread looks away. That is the
// "sound has broken up 8060 times" in BUGS.md point 5.
//
// So one frame goes in here and a duty thread takes it out a period at a time.
//
// This is a plain circular buffer of bytes with no locking and no ALSA: the
// ring's callers own the synchronisation and the feeder owns the writing. It is
// kept this way so the arithmetic — wraparound, the split copy, dropping the
// oldest to keep a delay from growing without bound — can be tested without a
// sound card. `tests/test_audio_ring.cpp` does that.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace multisite_player {

class AudioRing {
public:
    AudioRing() = default;

    // Resizing discards whatever was in it, which is what a fresh open wants.
    void reset(size_t capacity_bytes) {
        m_buf.assign(capacity_bytes, 0);
        m_head = m_used = 0;
    }

    size_t capacity() const { return m_buf.size(); }
    size_t used()     const { return m_used; }
    size_t room()     const { return m_buf.size() - m_used; }

    // Appends as much of `n` as fits and returns how many bytes that was. It
    // deliberately does not drop anything: whether to lose audio is the
    // caller's decision, and it is a decision about the picture's timing, not
    // the ring's.
    size_t push(const uint8_t* p, size_t n) {
        if (!p || n == 0 || m_buf.empty()) return 0;
        if (n > room()) n = room();
        if (n == 0) return 0;
        const size_t tail  = (m_head + m_used) % m_buf.size();
        const size_t first = std::min(n, m_buf.size() - tail);
        std::memcpy(m_buf.data() + tail, p, first);
        if (n > first) std::memcpy(m_buf.data(), p + first, n - first);
        m_used += n;
        return n;
    }

    // Reads up to `n` bytes out, oldest first, and returns how many it read.
    size_t pop(uint8_t* dst, size_t n) {
        if (!dst || n == 0 || m_used == 0) return 0;
        if (n > m_used) n = m_used;
        const size_t first = std::min(n, m_buf.size() - m_head);
        std::memcpy(dst, m_buf.data() + m_head, first);
        if (n > first) std::memcpy(dst + first, m_buf.data(), n - first);
        m_head = (m_head + n) % m_buf.size();
        m_used -= n;
        return n;
    }

    // Throws away the OLDEST `n` bytes, keeping the newest. This is what a
    // producer does when the ring is full: the alternative is to block the
    // thread that presents the picture, which makes the stall that filled the
    // ring worse, and the audio that would be lost is the audio that is already
    // furthest behind the picture. Returns how many bytes went.
    size_t drop_oldest(size_t n) {
        if (m_buf.empty() || n == 0 || m_used == 0) return 0;
        if (n > m_used) n = m_used;
        m_head = (m_head + n) % m_buf.size();
        m_used -= n;
        return n;
    }

    void clear() { m_head = m_used = 0; }

private:
    std::vector<uint8_t> m_buf;
    size_t m_head = 0;   // index of the oldest byte
    size_t m_used = 0;   // bytes held, from m_head onwards, wrapping
};

} // namespace multisite_player
