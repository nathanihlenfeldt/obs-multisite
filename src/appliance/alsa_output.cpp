// SPDX-License-Identifier: GPL-3.0-or-later
//
// alsa_output.cpp — production audio out of the box.
//
// The whole point of the audio design is that a satellite receives a
// production bus — main mix, mic ISOs, click — not a stereo listener feed. So
// this opens the device with the channel count the FEED carries and holds onto
// as many of the feed's channels as the card will accept.
//
// On the low-cost tier the device is HDMI, which carries up to eight channels
// of LPCM; a de-embedder at the campus recovers them. On the AES67 tier the
// device is a vendor sound card that takes 32-bit integers and cannot hold more
// than eight milliseconds of audio, which is where both halves of this file
// come from — see `pcm_convert.h` for the format, `open()` for the buffer, and
// the feeder thread below for why the writing cannot happen on the delivery
// thread.
//
#include "audio_output.h"
#include "log.h"
#include "sysinfo.h"   // to report why sound broke up, rather than guess
#include "pcm_convert.h"
#include "audio_ring.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

namespace {

// ── Why the writing does not happen on the delivery thread ───────────────────
//
// It used to: `Player`'s delivery thread called write() for each audio frame,
// and write() called snd_pcm_writei(). There are three things wrong with that,
// and the bench found all of them at once.
//
// A decoded frame is one AAC frame — 21 ms at 48 kHz, 32 ms for AC-3 — while
// the AES67 driver fixes the card's buffer at 8 ms and will not be told
// otherwise (BUGS.md point 5). So the write cannot return until 13 ms of that
// frame has drained, which means the delivery thread spends most of an audio
// frame inside a sound card. That thread also presents the picture, so every
// video present — up to a vblank away, and 100 ms if a flip never arrives —
// leaves the card with nothing to play. The bench logged an under-run on
// almost every video frame, roughly thirty a second, forever:
//
//     sound has broken up 8060 times — the board reports neither under-voltage
//     nor throttling, so this is most likely the feed or a burst of seeking
//
// It was not the feed and it was not seeking. It was a 21 ms producer writing
// to an 8 ms consumer on the same thread that drives the screen.
//
// So the writing moved off that thread. write() now converts and copies into a
// ring (audio_ring.h) and returns — it never touches ALSA, so it can never
// block the picture. A feeder thread drains the ring a period at a time, which
// is the size the card is happiest with and small enough that its own blocking
// is bounded by one millisecond.
//
// The cost is latency: the sound is delivered `audio_buffer_ms` behind the
// picture rather than as soon as it is decoded. That is the trade this makes —
// continuity for a known, adjustable delay — and it is why the ring depth is a
// setting rather than a constant.

class AlsaOutput : public AudioOutput {
public:
    ~AlsaOutput() override { close(); }

    bool open(const Config& cfg, int sample_rate, int channels,
              std::string& error) override;
    void close() override;
    bool ok() const override { return m_ok.load(); }

    std::string description() const override {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_description;
    }

    void write(const multisite::DecodedAudioFrame& frame) override;
    double delay_s() const override;
    void flush() override;
    std::vector<AudioDevice> devices() const override;

private:
    bool recover(int err);
    // Hands `frames` of already-formatted samples to the card, recovering from
    // an under-run without losing the rest of the buffer. Only the feeder
    // thread calls this.
    bool write_frames(const uint8_t* data, snd_pcm_uframes_t frames);

    // The feeder: drains m_ring into the card until asked to stop. Reads the
    // ALSA handle and m_period_frames, which open() sets before this starts and
    // close() stops this before clearing.
    void feed();

    // The body of close(), without taking m_lifecycle_mtx — so open() can call
    // it while already holding that lock instead of deadlocking on a
    // non-recursive mutex.
    void close_locked();

    // Where m_feed_thread is. Read from the player's status path without taking
    // the lock the feeder holds while it writes.
    std::atomic<bool> m_ok{false};

    // Serialises open() and close() against EACH OTHER. They are not called from
    // one thread: the delivery loop opens and closes the output, and the control
    // API's reconfigure() also closes it — to silence a box the moment audio is
    // switched off — on the HTTP thread. Without this, two closers could each
    // find the feeder joinable and both call join() on it, which is undefined
    // behaviour, and it is one tap away in the web interface's audio switch.
    //
    // Held for the whole of open() and close(). Never taken by write(),
    // delay_s(), flush() or the feeder, so it cannot put the picture behind the
    // sound card.
    std::mutex m_lifecycle_mtx;

    mutable std::mutex m_mtx;
    snd_pcm_t*  m_pcm = nullptr;
    int         m_rate = 48000;
    int         m_channels = 2;
    // Which of float/S32/S16 the card agreed to. Float is the decoder's own
    // layout and costs nothing; the others are converted on the way out.
    PcmOutFormat m_format = PcmOutFormat::Float32;
    std::string m_description = "no audio output";
    // Rate-limit the complaint: a card that keeps under-running must not fill
    // the log faster than it fills its buffer.
    long long   m_xruns = 0;
    long long   m_logged_xruns = 0;
    // Scratch for the converted samples, kept between writes so the thread that
    // presents the picture is not also reallocating a buffer every frame.
    std::vector<uint8_t> m_bytes;

    // ── The ring and the thread that drains it ───────────────────────────────
    // Written by the delivery thread in write(), read by the feeder. Its own
    // mutex, never held while calling into ALSA, so a write never waits on the
    // sound card.
    AudioRing            m_ring;
    // Mutable because delay_s() is const and needs to read the queue's depth.
    mutable std::mutex   m_ring_mtx;
    // Woken when there is something to play or when close() wants the feeder
    // to stop. Its wait uses m_ring_mtx, which is also what protects the queue
    // it is waiting on, so there is no separate mutex for it.
    std::condition_variable m_cv;
    std::thread          m_feed_thread;
    std::atomic<bool>    m_stop{false};
    // Set by flush() to make the feeder fill the queue to its target again
    // before it resumes writing. Without it, a seek would leave the queue empty
    // while the feeder still believed playback had started, so it would hand the
    // card one millisecond and then starve it until the next frame arrived — a
    // gap on every jump, and a fresh batch of the under-run counts this whole
    // arrangement exists to remove. A seek is already a break; it should not
    // also be one you can hear.
    std::atomic<bool>    m_reprime{false};
    // Bytes in one frame at the card's format and channel count: what turns
    // milliseconds into ring bytes and a period into a frame count.
    size_t               m_frame_bytes = 0;
    // One period's worth of frames — a millisecond on the AES67 card, whatever
    // the driver agreed to elsewhere. The feeder writes this much at a time.
    snd_pcm_uframes_t    m_period_frames = 0;
    // Where a fresh write starts playing from, in ring bytes. Set once by
    // open(); it is the depth that decides the trade described above the class.
    size_t               m_target_bytes = 0;
    // Whether the ring is in use at all. It is engaged only when the card's
    // buffer is smaller than a decoded frame, which is the one case that needs
    // it; see the decision in open(). False means write() goes straight to the
    // card, exactly as it always did.
    //
    // Atomic because close() writes it from the HTTP thread (reconfigure(),
    // switching audio off in the interface) while the delivery thread is
    // reading it in write(). The value can only flip false, and a write that
    // sees a stale `true` only pushes into a ring nobody drains — the samples
    // are dropped when it next opens, which is what should happen to audio from
    // before the change anyway. Reading it unlocked is the point: taking m_mtx
    // here would put this thread back behind the feeder's blocking write, which
    // is the whole fault being fixed.
    std::atomic<bool>    m_use_ring{false};
};

bool AlsaOutput::open(const Config& cfg, int sample_rate, int channels,
                      std::string& error) {
    // Against close() from the control API's HTTP thread, not just this one.
    std::lock_guard<std::mutex> life(m_lifecycle_mtx);
    close_locked();
    std::lock_guard<std::mutex> lk(m_mtx);
    error.clear();

    const std::string device = cfg.alsa_device.empty() ? "default"
                                                       : cfg.alsa_device;
    int rc = snd_pcm_open(&m_pcm, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        error = "cannot open " + device + ": " + snd_strerror(rc);
        m_pcm = nullptr;
        return false;
    }

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(m_pcm, hw);
    snd_pcm_hw_params_set_access(m_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);

    // Ask for what the decoder produces, then for signed integers if the card
    // will not have it. Float arrives with no conversion at all, which is why
    // it is first; a card that refuses it (the AES67 driver advertises
    // S32_LE only) used to end the open here with a message an operator could
    // do nothing about. test_format() is used rather than set_format() so a
    // refusal cannot disturb the parameters the retry is built on.
    static const struct { snd_pcm_format_t alsa; PcmOutFormat ours; } kFormats[] = {
        { SND_PCM_FORMAT_FLOAT_LE, PcmOutFormat::Float32 },
        { SND_PCM_FORMAT_S32_LE,   PcmOutFormat::S32     },
        { SND_PCM_FORMAT_S16_LE,   PcmOutFormat::S16     },
    };
    bool format_ok = false;
    for (const auto& cand : kFormats) {
        if (snd_pcm_hw_params_test_format(m_pcm, hw, cand.alsa) == 0) {
            snd_pcm_hw_params_set_format(m_pcm, hw, cand.alsa);
            m_format = cand.ours;
            format_ok = true;
            break;
        }
    }
    if (!format_ok) {
        // Name what the card does offer. Being told the card takes none of
        // float, S32 or S16 is only useful if the next line says what it wants
        // instead, and that is one call away.
        snd_pcm_format_mask_t* mask = nullptr;
        snd_pcm_format_mask_alloca(&mask);
        snd_pcm_hw_params_get_format_mask(hw, mask);
        std::string offers;
        for (int f = 0; f <= SND_PCM_FORMAT_LAST; ++f) {
            if (snd_pcm_format_mask_test(mask, (snd_pcm_format_t)f)) {
                if (!offers.empty()) offers += ", ";
                offers += snd_pcm_format_name((snd_pcm_format_t)f);
            }
        }
        error = device + " takes none of the formats this player can send "
                "(float, 32-bit or 16-bit PCM). It offers: " +
                (offers.empty() ? "nothing ALSA recognises" : offers);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }
    if (m_format != PcmOutFormat::Float32)
        plog_info("%s takes no floating-point audio — sending %s instead",
                  device.c_str(), pcm_format_name(m_format));

    unsigned want = (unsigned)std::max(1, channels);
    rc = snd_pcm_hw_params_set_channels(m_pcm, hw, want);
    if (rc < 0) {
        unsigned got = want;
        if (snd_pcm_hw_params_set_channels_near(m_pcm, hw, &got) < 0 ||
            got == 0) {
            error = device + " will not take " + std::to_string(want) +
                    " channels: " + snd_strerror(rc);
            snd_pcm_close(m_pcm);
            m_pcm = nullptr;
            return false;
        }
        // This is worth shouting about. A campus that thinks it is receiving
        // a click track and is not will only find out during an event.
        plog_error("%s will only take %u channels but the feed carries %d — "
                   "the extra channels are NOT being played. Check the output "
                   "device, or use an HDMI de-embedder that takes all eight.",
                   device.c_str(), got, channels);
        want = got;
    }
    m_channels = (int)want;

    unsigned rate = (unsigned)sample_rate;
    rc = snd_pcm_hw_params_set_rate_near(m_pcm, hw, &rate, nullptr);
    if (rc < 0) {
        error = device + " will not run at " + std::to_string(sample_rate) +
                " Hz: " + snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }
    if ((int)rate != sample_rate)
        plog_warn("%s is running at %u Hz, not the feed's %d Hz",
                  device.c_str(), rate, sample_rate);
    m_rate = (int)rate;

    // Half a second of buffer. Generous on purpose: the delivery thread paces
    // frames against a monotonic clock, and a card whose own clock runs a few
    // parts per million away from it needs somewhere for that difference to go
    // over a two-hour event.
    //
    // The figure wanted is kept separately because set_buffer_time_near writes
    // the *achievable* value back into the variable it is given; comparing that
    // against itself later would report that every card gave exactly what was
    // asked for, on the one install where it matters.
    const unsigned buffer_us_asked = 500000;
    unsigned buffer_us = buffer_us_asked;
    snd_pcm_hw_params_set_buffer_time_near(m_pcm, hw, &buffer_us, nullptr);
    unsigned period_us = 40000;
    snd_pcm_hw_params_set_period_time_near(m_pcm, hw, &period_us, nullptr);

    rc = snd_pcm_hw_params(m_pcm, hw);
    if (rc < 0) {
        error = std::string("the sound card refused those settings: ") +
                snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    snd_pcm_sw_params_t* sw = nullptr;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(m_pcm, sw);
    snd_pcm_uframes_t buffer_size = 0, period_size = 0;
    snd_pcm_get_params(m_pcm, &buffer_size, &period_size);

    // What the card actually granted, rather than what was asked for. A driver
    // is free to clamp both — the AES67 driver pins period_bytes_min and
    // period_bytes_max to one millisecond, and periods_min and periods_max to
    // its own bufMs, so the half-second requested below becomes 8 ms and cannot
    // be made larger. Nothing said so, and that silence cost a bench afternoon:
    // this line is now the one place it gets reported.
    {
        const unsigned rate = (unsigned)(m_rate > 0 ? m_rate : 1);
        const double granted_ms = (double)buffer_size * 1000.0 / (double)rate;
        const double period_ms  = (double)period_size  * 1000.0 / (double)rate;
        const double asked_ms   = (double)buffer_us_asked / 1000.0;
        m_description = std::string(device) + ", " +
                        std::to_string(m_channels) +
                        (m_channels == 1 ? " channel at " : " channels at ") +
                        std::to_string(m_rate) + " Hz, " +
                        pcm_format_name(m_format);
        if (granted_ms < asked_ms / 2.0) {
            plog_warn("%s gave a %.0f ms buffer, not the %.0f ms asked for "
                      "(%.0f ms of it per period, %d periods). A decoded frame "
                      "is longer than that, so the player holds its own queue "
                      "in front of this one; audio_buffer_ms sets how much.",
                      device.c_str(), granted_ms, asked_ms, period_ms,
                      (int)(buffer_size / (period_size ? period_size : 1)));
        }

        // ── Does this card need a queue in front of it? ──────────────────────
        // Only if its buffer cannot hold a whole decoded frame. HDMI grants
        // half a second, so a write never has to wait and there is nothing to
        // decouple — putting 60 ms of queue in front of it would buy jitter
        // absorption that card does not need, and pay for it in lip sync. The
        // AES67 card grants 8 ms against a 21 ms frame, and that is the case
        // this exists for. The threshold is two typical AAC frames: a card that
        // can hold more than one frame is not the problem being solved.
        m_use_ring = granted_ms < 40.0;
    }

    // Start once there is a period banked, so the first write does not play
    // out into a half-empty buffer and under-run immediately.
    snd_pcm_sw_params_set_start_threshold(m_pcm, sw, period_size);
    snd_pcm_sw_params_set_avail_min(m_pcm, sw, period_size);
    snd_pcm_sw_params(m_pcm, sw);

    if ((rc = snd_pcm_prepare(m_pcm)) < 0) {
        error = std::string("could not start the sound card: ") + snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

    m_description += ", " + std::to_string((int)(buffer_size * 1000ULL /
                                    (unsigned)(m_rate > 0 ? m_rate : 1))) +
                     " ms buffer";
    m_xruns = m_logged_xruns = 0;
    m_bytes.clear();

    // Bytes in one frame at the card's format and channel count: what turns
    // milliseconds into queue bytes and a period into a frame count.
    m_frame_bytes = (size_t)m_channels *
                    (size_t)pcm_bytes_per_sample(m_format);
    if (m_frame_bytes == 0) {
        error = "the sound card agreed to no channels";
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }
    m_period_frames = period_size ? period_size : 48;

    // ── The queue in front of the card, when the card needs one ──────────────
    //
    // Everything above this line is the card. Everything below is the ring the
    // delivery thread writes into and the feeder drains; see the block comment
    // above the class for why the two are separate. On a card whose buffer
    // already holds a frame (m_use_ring false) none of this is set up, write()
    // goes straight to the card, and delay_s() reports the card alone — the
    // behaviour every existing install already had.
    if (m_use_ring) {
        // One period at a time is what the feeder writes. It is also the card's
        // own period, so each call into ALSA has a millisecond of work to do
        // and returns; the ring is what absorbs the difference between that and
        // a frame's worth of audio arriving at once.
        //
        // The queue is four times the target so that reaching the target starts
        // playback and the rest is headroom for the delivery thread falling
        // behind — a seek, a decoder restart, a stall. Beyond that the oldest
        // audio is dropped, because holding it would only put the sound further
        // behind the picture, and late audio is worse than absent audio.
        int want_ms = cfg.audio_buffer_ms > 0 ? cfg.audio_buffer_ms : 60;
        if (want_ms > 2000) want_ms = 2000;
        m_target_bytes = (size_t)m_rate * m_frame_bytes * (size_t)want_ms / 1000;
        // At least two periods, so the feeder always has a whole period to take
        // and cannot spin on a target smaller than one write.
        const size_t floor_bytes = (size_t)m_period_frames * m_frame_bytes * 2;
        if (m_target_bytes < floor_bytes) m_target_bytes = floor_bytes;
        m_ring.reset(m_target_bytes * 4);

        m_description += ", " + std::to_string(want_ms) + " ms queue";
        plog_info("audio queue: %d ms in %zu bytes, fed one %u-frame period at "
                  "a time", want_ms, m_ring.capacity(), (unsigned)m_period_frames);

        m_stop.store(false);
        m_feed_thread = std::thread([this] { feed(); });
    }

    m_ok.store(true);
    return true;
}

void AlsaOutput::close() {
    // Against open(), and against a second close().
    //
    // Both really happen from different threads: the delivery loop closes the
    // output when the device or the channel count changes, and reconfigure()
    // closes it from the HTTP thread to silence the box the instant audio is
    // switched off in the interface. Two threads that both find the feeder
    // joinable would both call join() on the same std::thread, which is
    // undefined behaviour — and this is one tap away in the web interface.
    // Serialised here, the second caller finds nothing to join.
    std::lock_guard<std::mutex> life(m_lifecycle_mtx);
    close_locked();
}

void AlsaOutput::close_locked() {
    // Stop the feeder before the card goes, so it cannot be half-way through a
    // write when the handle it is holding is closed underneath it.
    if (m_feed_thread.joinable()) {
        m_stop.store(true);
        m_cv.notify_all();
        m_feed_thread.join();
    }

    std::lock_guard<std::mutex> lk(m_mtx);
    m_use_ring = false;
    if (m_ring.capacity()) {
        std::lock_guard<std::mutex> rl(m_ring_mtx);
        m_ring.clear();
    }
    if (!m_pcm) return;
    snd_pcm_drop(m_pcm);
    snd_pcm_close(m_pcm);
    m_pcm = nullptr;
    m_description = "no audio output";
}

bool AlsaOutput::recover(int err) {
    // An under-run means the card ran out of samples — worth counting, because
    // it is audible. It does NOT mean the box is struggling: the most common
    // cause found so far is a card whose buffer is smaller than one decoded
    // frame, which is a property of the driver rather than of the load. A
    // thermally throttled machine is the other cause, and the only one the
    // message used to consider. Recovery is silent; the count is not.
    if (err == -EPIPE) ++m_xruns;
    const int rc = snd_pcm_recover(m_pcm, err, 1 /* silent */);
    if (rc < 0) {
        plog_error("sound card stopped: %s", snd_strerror(rc));
        return false;
    }
    if (m_xruns - m_logged_xruns >= 10) {
        m_logged_xruns = m_xruns;
        // Say what the box reports rather than guessing at it. This used to
        // blame heat or power unconditionally, and the one time it fired in
        // the field it did so straight after five decoder restarts inside
        // thirteen seconds — somebody scrubbing the timeline, not a hot
        // heatsink. The Pi publishes both conditions, so ask.
        const SystemInfo sys = system_info();
        const char* why =
              (sys.under_voltage && sys.throttled)
                  ? " — the power supply is not keeping up and the board is "
                    "throttling"
            : sys.under_voltage
                  ? " — the board reports under-voltage, so suspect the power "
                    "supply or cable"
            : sys.throttled
                  ? " — the board is throttling, so suspect cooling"
            : " — the board reports neither under-voltage nor throttling, so "
              "this is not the hardware. If the card's buffer is smaller than a "
              "decoded frame (the log says so as it opens), suspect that first; "
              "otherwise it is the feed or a burst of seeking";
        plog_warn("sound has broken up %lld times%s", m_xruns, why);
    }
    return true;
}

void AlsaOutput::write(const multisite::DecodedAudioFrame& frame) {
    if (frame.frames == 0 || frame.interleaved.empty()) return;

    // The card's format, channel count and whether there is a queue at all are
    // fixed by open() and cannot change while it is open, so they are read here
    // without the lock deliberately: taking m_mtx would put this thread back
    // behind the feeder's blocking write, which is the very thing the queue
    // exists to stop. open() and close() run on this same thread.
    const PcmOutFormat fmt      = m_format;
    const int           channels = m_channels;
    const bool          via_ring = m_use_ring;
    const size_t frame_bytes = (size_t)channels *
                               (size_t)pcm_bytes_per_sample(fmt);
    if (frame_bytes == 0) return;

    // Straight through only when the card took float *and* the feed's channel
    // count already matches the device: the decoder's own buffer is then
    // exactly what ALSA is waiting for, and copying every sample on the thread
    // that also presents the picture is worth avoiding. Otherwise convert: to
    // integers, or to the channel count the card will take, or both. The policy
    // is the one this always used — play what the card will take rather than
    // nothing at all, having said so in the log when whole channels are dropped.
    const uint8_t* data;
    size_t bytes;
    if (fmt == PcmOutFormat::Float32 && frame.channels == channels) {
        data  = reinterpret_cast<const uint8_t*>(frame.interleaved.data());
        bytes = (size_t)frame.frames * frame_bytes;
    } else {
        pcm_convert(frame.interleaved.data(), frame.frames, frame.channels,
                    channels, fmt, m_bytes);
        data  = m_bytes.data();
        bytes = m_bytes.size();
    }
    if (bytes == 0) return;

    if (!via_ring) {
        // A card whose own buffer holds a whole frame — HDMI. Unchanged.
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_pcm) return;
        if (!write_frames(data, bytes / frame_bytes)) m_ok.store(false);
        return;
    }

    // The queue. Nothing below touches ALSA, so nothing below can block the
    // thread that presents the picture — it copies and returns, and the feeder
    // takes it out a period at a time.
    size_t pushed = 0;
    {
        std::lock_guard<std::mutex> rl(m_ring_mtx);
        pushed = m_ring.push(data, bytes);
        if (pushed < bytes) {
            // Full. Losing the OLDEST audio is the deliberate choice: the
            // alternative is to block here, which makes the stall that filled
            // the queue worse, and the audio that goes is the audio already
            // furthest behind the picture.
            const size_t overflow = bytes - pushed;
            m_ring.drop_oldest(overflow);
            pushed += m_ring.push(data + pushed, overflow);
        }
    }
    if (pushed) m_cv.notify_one();
}

// The feeder thread. Takes the queue out a period at a time — the card's own
// period, so each call into ALSA has a millisecond of work and returns — which
// is what turns a 21 ms frame arriving in bursts into an even supply for an
// 8 ms card. It is the ONLY caller of write_frames() while the queue is in use.
void AlsaOutput::feed() {
    const size_t frame_bytes = m_frame_bytes;
    if (frame_bytes == 0 || m_period_frames == 0) return;
    const size_t period_bytes = (size_t)m_period_frames * frame_bytes;

    std::vector<uint8_t> chunk(period_bytes);
    // Whether playback has started. Before it has, the queue is filled to its
    // target first: the first write otherwise plays into a nearly empty card
    // and under-runs immediately, which is the fault being fixed. After that a
    // single period is enough — the feeder's job is to keep the card fed, not
    // to hold the queue at the target for its own sake.
    bool primed = false;

    while (!m_stop.load()) {
        // A seek empties the queue behind our back; fill it again before
        // resuming, rather than trickling into a card about to run dry.
        if (m_reprime.exchange(false)) primed = false;

        size_t got = 0;
        {
            std::unique_lock<std::mutex> rl(m_ring_mtx);
            const size_t need = primed ? period_bytes : m_target_bytes;
            m_cv.wait_for(rl, std::chrono::milliseconds(200), [&] {
                return m_stop.load() || m_ring.used() >= need;
            });
            if (m_stop.load()) return;
            if (m_ring.used() < need) continue;
            got = m_ring.pop(chunk.data(), period_bytes);
            if (got >= period_bytes) primed = true;
        }
        if (got == 0) continue;

        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_pcm) return;
        if (!write_frames(chunk.data(), (snd_pcm_uframes_t)(got / frame_bytes))) {
            // The card stopped and could not be recovered. Say so through ok()
            // rather than spinning: the status line and the interface read it.
            m_ok.store(false);
            return;
        }
    }
}

bool AlsaOutput::write_frames(const uint8_t* data, snd_pcm_uframes_t frames) {
    if (!m_pcm) return false;
    const size_t frame_bytes = (size_t)m_channels *
                               (size_t)pcm_bytes_per_sample(m_format);
    if (frame_bytes == 0) return false;

    snd_pcm_uframes_t remaining = frames;
    while (remaining > 0) {
        const snd_pcm_sframes_t wrote = snd_pcm_writei(m_pcm, data, remaining);
        if (wrote < 0) {
            if (!recover((int)wrote)) {
                snd_pcm_close(m_pcm);
                m_pcm = nullptr;
                return false;
            }
            continue;
        }
        data += (size_t)wrote * frame_bytes;
        remaining -= (snd_pcm_uframes_t)wrote;
    }
    return true;
}

double AlsaOutput::delay_s() const {
    // Everything written but not yet heard, which is now two things: what the
    // card is still to play, and what is sitting in the queue in front of it.
    // Reporting only the card would understate the latency by the whole queue
    // depth, which is exactly the number any lip-sync correction depends on.
    //
    // The queue is read under its own lock and released before m_mtx is taken,
    // in the same order the feeder uses, so nothing here can deadlock with it.
    double queued_s = 0.0;
    if (m_use_ring && m_frame_bytes > 0 && m_rate > 0) {
        std::lock_guard<std::mutex> rl(m_ring_mtx);
        queued_s = (double)m_ring.used() /
                   ((double)m_frame_bytes * (double)m_rate);
    }

    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return queued_s;
    snd_pcm_sframes_t frames = 0;
    if (snd_pcm_delay(m_pcm, &frames) < 0 || frames < 0) return queued_s;
    return queued_s + (double)frames / (double)m_rate;
}

void AlsaOutput::flush() {
    // After a jump, whatever is buffered belongs to where playback used to be —
    // in the card and in the queue in front of it. The queue is cleared under
    // its own lock and the card under m_mtx; the feeder takes them in that same
    // order, so the two cannot deadlock.
    //
    // The feeder is also told to fill the queue again before it resumes: it is
    // about to be handed an empty card, and one millisecond written into that
    // would under-run before the next frame could arrive.
    if (m_use_ring) {
        m_reprime.store(true);
        std::lock_guard<std::mutex> rl(m_ring_mtx);
        m_ring.clear();
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return;
    snd_pcm_drop(m_pcm);
    snd_pcm_prepare(m_pcm);
}

std::vector<AudioDevice> AlsaOutput::devices() const {
    std::vector<AudioDevice> out;
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) != 0) return out;

    for (void** h = hints; h && *h; ++h) {
        char* name = snd_device_name_get_hint(*h, "NAME");
        char* desc = snd_device_name_get_hint(*h, "DESC");
        char* io   = snd_device_name_get_hint(*h, "IOID");

        // Playback devices only, and not the dozens of plugin aliases: an
        // operator picking an output should see the sockets on the box, not
        // ALSA's internal plumbing.
        const bool playback = !io || std::strcmp(io, "Output") == 0;
        const std::string id = name ? name : "";
        const bool interesting =
            playback && !id.empty() &&
            (id == "default" || id.rfind("hw:", 0) == 0 ||
             id.rfind("plughw:", 0) == 0 || id.rfind("sysdefault:", 0) == 0);

        if (interesting) {
            AudioDevice d;
            d.id = id;
            std::string text = desc ? desc : id;
            // Hints put the human name on a second line; join it up.
            std::replace(text.begin(), text.end(), '\n', ' ');
            d.description = text;
            out.push_back(std::move(d));
        }
        if (name) ::free(name);
        if (desc) ::free(desc);
        if (io)   ::free(io);
    }
    snd_device_name_free_hint(hints);
    return out;
}

} // namespace

AudioOutput* make_alsa_output() { return new AlsaOutput(); }

} // namespace multisite_player
