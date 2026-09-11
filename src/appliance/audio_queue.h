// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// audio_queue.h — the decoded audio waiting to be heard.
//
// On HDMI the sound card does this job itself: ALSA's ring absorbs the
// difference between how fast a decoder produces frames and how fast a card
// consumes 1 ms periods, and nothing on this side has to know or care.
//
// The AES67 card is not like that. Its buffer is a calendar of one-millisecond
// slots, `bufMs` (eight) deep, and the daemon reads the slot for the
// millisecond its clock currently shows — it does not wait for anyone. A
// decoded frame is about twenty-one milliseconds, so it cannot go into the
// calendar in one piece, and it cannot be handed to it late either. Something
// has to hold the difference: this queue.
//
// It also has to be somewhere the thread that presents the picture can put
// audio without ever waiting. That thread writes a frame and returns; the
// feeder thread takes one millisecond out at a time and drops it into the
// calendar slot the daemon is about to read. Neither of them waits on the
// other for longer than a memcpy.
//
// The arithmetic is the part that can be quietly wrong — a wraparound index
// that is off by one loses a millisecond of audio every time the buffer turns
// over, which is a click every 60 ms and very hard to trace back — so it lives
// in a header with no ALSA and no threads in it, where `tests/test_audio_queue.cpp`
// can reach it. The locking is left to the owner, which is why there is none
// here: a class that locks internally cannot be tested without threads.
#include <cstdint>
#include <cstring>
#include <vector>

namespace multisite_player {

// A bounded queue of bytes, oldest out first.
class ByteQueue {
public:
    // Size it once, when the device is opened and its rate is known.
    void reset(size_t capacity) {
        m_buf.assign(capacity, 0);
        m_cap = capacity;
        m_head = m_tail = m_size = 0;
    }

    // Throw away what is held but keep the capacity — after a seek, so the
    // sound does not run on from where playback used to be.
    void clear() { m_head = m_tail = m_size = 0; }

    size_t capacity() const { return m_cap; }
    size_t size() const { return m_size; }
    bool   empty() const { return m_size == 0; }

    // Appends as much of `n` bytes as fits, returning how many were taken. A
    // short push is reported rather than hidden: it means the queue has fallen
    // behind, and the honest response is to say so, because the alternative —
    // waiting for room — would stall the thread that is showing the picture.
    size_t push(const uint8_t* data, size_t n) {
        if (m_cap == 0 || n == 0) return 0;
        const size_t room = m_cap - m_size;
        const size_t take = n < room ? n : room;
        for (size_t i = 0; i < take; ++i) {
            m_buf[m_tail] = data[i];
            m_tail = (m_tail + 1) % m_cap;
        }
        m_size += take;
        return take;
    }

    // Takes up to `n` bytes out, oldest first, returning how many were there.
    // Fewer than `n` means the caller is about to run dry — for the calendar
    // feeder that is a slot it has to pad, and it is worth counting.
    size_t pop(uint8_t* out, size_t n) {
        const size_t take = n < m_size ? n : m_size;
        for (size_t i = 0; i < take; ++i) {
            out[i] = m_buf[m_head];
            m_head = (m_head + 1) % m_cap;
        }
        m_size -= take;
        return take;
    }

    // Drops the oldest `n` bytes without copying them out.
    size_t discard(size_t n) {
        const size_t take = n < m_size ? n : m_size;
        m_head = (m_head + take) % (m_cap ? m_cap : 1);
        m_size -= take;
        return take;
    }

private:
    std::vector<uint8_t> m_buf;
    size_t m_cap  = 0;
    size_t m_head = 0;   // next byte to pop
    size_t m_tail = 0;   // next byte to push
    size_t m_size = 0;   // bytes held
};

} // namespace multisite_player
