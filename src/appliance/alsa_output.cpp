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
// device is a vendor sound card that takes 32-bit integers and five-millisecond
// periods, which is where both halves of the format negotiation below come
// from — see `pcm_convert.h` for the first and `open()` for the second.
//
#include "audio_output.h"
#include "log.h"
#include "sysinfo.h"   // to report why sound broke up, rather than guess
#include "pcm_convert.h"
// The AES67 card is not a queue on a clock we control, so its audio is not
// handed to ALSA at all — it is addressed into the daemon's own calendar. See
// `digisyn_calendar.h` for the addressing and `audio_queue.h` for what holds
// the difference between a decoded frame and a 1 ms slot. Both are free of ALSA
// so they can be tested off the appliance.
#include "audio_queue.h"
#include "digisyn_calendar.h"

#include <alsa/asoundlib.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

namespace {

// The vendor daemon posts a header and a calendar of playback slots into this
// device's mapping. Its layout is in `digisyn_calendar.h`.
constexpr const char* kDigisynDevice = "/dev/Digisyn_vSndCard";

// How much decoded audio to hold in front of the calendar. This is the
// continuity-versus-lip-sync trade: 60 ms absorbs the jitter between a decoder
// producing a frame at a time and a card consuming a millisecond at a time,
// without being visible on lips. It is the same depth the ALSA path aims for.
constexpr int kDigisynQueueMs = 60;

// Does this string name the Digisyn card?
//
// "Digisyn" alone is the test, because the vendor truncates in three different
// places: the card driver is "Digisyn_vSndCard", the shortname is
// "Digisyn_vSndCar" (fifteen characters, ALSA's limit), and the card's own id —
// what a device string carries after CARD= — is "Default". Matching any part of
// the full name is the only test that survives all of them.
bool name_mentions_digisyn(const char* s) {
    return s != nullptr && std::strstr(s, "Digisyn") != nullptr;
}

// The card id a device string asks for, if it asks for one:
// "sysdefault:CARD=Default" → "Default", "hw:CARD=Digisyn_vSndCar,DEV=0" →
// "Digisyn_vSndCar", "default" → "".
std::string card_id_in(const std::string& device) {
    static const char key[] = "CARD=";
    const size_t at = device.find(key);
    if (at == std::string::npos) return std::string();
    const size_t from = at + sizeof(key) - 1;
    const size_t to = device.find_first_of(",:", from);
    const size_t count =
        (to == std::string::npos) ? std::string::npos : to - from;
    return device.substr(from, count);
}

// Is the card that answers to this id the vendor's? Walked by number, so the
// answer comes out of the card itself and never out of the string alone.
bool card_id_is_digisyn(const std::string& card_id) {
    if (card_id.empty()) return false;
    for (int n = 0; n < 32; ++n) {
        char ctl_name[32];
        std::snprintf(ctl_name, sizeof ctl_name, "hw:%d", n);
        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;   // no such card
        snd_ctl_card_info_t* ci = nullptr;
        snd_ctl_card_info_alloca(&ci);
        const bool readable = snd_ctl_card_info(ctl, ci) >= 0;
        const char* id = readable ? snd_ctl_card_info_get_id(ci) : nullptr;
        const bool match =
            id != nullptr && card_id == id &&
            (name_mentions_digisyn(snd_ctl_card_info_get_driver(ci)) ||
             name_mentions_digisyn(snd_ctl_card_info_get_name(ci)) ||
             name_mentions_digisyn(snd_ctl_card_info_get_longname(ci)));
        snd_ctl_close(ctl);
        if (match) return true;
    }
    return false;
}

// Why this PCM is the Digisyn AES67 card — or nullptr if it is not.
//
// Asked of the card rather than matched against the config string, so hw:,
// plughw:, sysdefault: and default all take the same branch while HDMI and
// every other card keep the plain ALSA path.
//
// Three answers are tried, because the first two can be hidden by a plugin:
//
//   1. the PCM's *id* — the vendor's own string, "Digisyn_vSndCard PCM".
//   2. the *card* behind the PCM, through its control interface.
//   3. failing both — and `sysdefault:CARD=…` is the case that does, the one the
//      bench opened — the card id written in the device string, looked up.
//
// The PCM's *name* is deliberately not consulted, and was this function's first
// version: the vendor sets it with `strcpy(pcm->name, "Dummy PCM")`
// (DigiAes67KoLib/Digisyn-vSndCard.c), so the check never matched, the calendar
// was never used and the log said nothing about it — a whole session of "broken
// up" with no calendar line in it, which is exactly how it failed on the bench.
//
// The reason is returned rather than a bare yes so that this cannot fail
// silently again: `open()` logs which answer matched.
const char* digisyn_reason(snd_pcm_t* pcm, const std::string& device) {
    snd_pcm_info_t* info = nullptr;
    snd_pcm_info_alloca(&info);
    const bool have_info = snd_pcm_info(pcm, info) >= 0;

    if (have_info && name_mentions_digisyn(snd_pcm_info_get_id(info)))
        return "the PCM carries the vendor's own id";

    if (have_info) {
        const int card = snd_pcm_info_get_card(info);
        if (card >= 0) {
            char ctl_name[32];
            std::snprintf(ctl_name, sizeof ctl_name, "hw:%d", card);
            snd_ctl_t* ctl = nullptr;
            if (snd_ctl_open(&ctl, ctl_name, 0) == 0) {
                snd_ctl_card_info_t* ci = nullptr;
                snd_ctl_card_info_alloca(&ci);
                const bool match =
                    snd_ctl_card_info(ctl, ci) >= 0 &&
                    (name_mentions_digisyn(snd_ctl_card_info_get_driver(ci)) ||
                     name_mentions_digisyn(snd_ctl_card_info_get_name(ci)) ||
                     name_mentions_digisyn(snd_ctl_card_info_get_longname(ci)));
                snd_ctl_close(ctl);
                if (match) return "the card behind the PCM is the vendor's";
            }
        }
    }

    if (card_id_is_digisyn(card_id_in(device)))
        return "the device string names the vendor's card";
    return nullptr;
}

class AlsaOutput : public AudioOutput {
public:
    ~AlsaOutput() override { close(); }

    bool open(const Config& cfg, int sample_rate, int channels,
              std::string& error) override;
    void close() override;
    bool ok() const override { return m_pcm != nullptr || m_map != nullptr; }

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
    // an under-run without losing the rest of the buffer.
    bool write_frames(const uint8_t* data, snd_pcm_uframes_t frames);

    // Open the AES67 card's calendar instead of the PCM. Returns false, with a
    // reason, if the device is absent or does not look like one — the caller
    // then falls back to ALSA rather than going silent. `feed_channels` is what
    // the stream carries, which the calendar's own channel count is reported
    // against.
    bool open_calendar(int feed_channels, std::string& error);
    // Put one decoded frame's audio into the calendar's queue — the whole of the
    // write path when the AES67 card is selected.
    void write_calendar(const multisite::DecodedAudioFrame& frame);
    // Hand converted bytes to the queue the feeder drains. Never blocks: a full
    // queue drops its oldest audio instead of stalling the thread that presents
    // the picture. Its own lock, because the feeder takes it too and must not
    // wait on the one `close()` holds while joining.
    void queue_bytes(const uint8_t* data, size_t n);
    // The feeder thread: fills calendar slots ahead of the daemon's clock.
    void feed();
    // Fill one millisecond's slot, with silence for whatever is not queued yet.
    // Feeder thread only, so it takes no lock on the mapping.
    void write_slot(const DigisynHeader& h, uint64_t ms_index);

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
    // Channels the feed carries, when the device would not take them all.
    int         m_source_channels = 0;
    // Scratch for the converted samples, kept between writes so the thread that
    // presents the picture is not also reallocating a buffer every frame.
    std::vector<uint8_t> m_bytes;

    // ── The AES67 calendar (see digisyn_calendar.h) ──────────────────────────
    // True when the audio is going into the daemon's calendar rather than
    // through ALSA. m_pcm is then null: nothing is written to the PCM. Atomic
    // because close() can run on a different thread than the one writing audio.
    std::atomic<bool> m_calendar{false};
    void*       m_map = nullptr;        // the mmap'd header + calendars
    size_t      m_map_size = 0;
    DigisynHeader* m_hdr = nullptr;     // into m_map; msIndex is read live
    size_t      m_slot_bytes = 0;       // one millisecond of the calendar
    // One slot's worth of scratch for the feeder, so it is not allocating while
    // the daemon is reading.
    std::vector<uint8_t> m_slot_buf;
    // The buffer between decoded frames (~21 ms) and this card's 1 ms slots.
    ByteQueue   m_queue;
    mutable std::mutex m_queue_mtx;
    // The next millisecond whose slot has not yet been written. Owned by the
    // feeder; written by flush() on the delivery thread, hence atomic. Zero
    // means "not started".
    std::atomic<uint64_t> m_next_ms{0};
    std::thread m_feeder;
    std::atomic<bool> m_stop{false};
    // So exactly one caller joins the feeder. close() is reachable from the
    // delivery thread and from the HTTP thread (the operator switching audio
    // off), and a second join() of the same thread is undefined behaviour.
    std::atomic<bool> m_feeder_started{false};
    // Slots filled with silence (the queue ran dry) and bytes dropped for
    // running ahead. Both are the audible failure modes of the calendar, so
    // both are counted; the feeder and the writer are different threads.
    std::atomic<long long> m_starved{0};
    std::atomic<long long> m_logged_starved{0};
    std::atomic<long long> m_dropped{0};
};

bool AlsaOutput::open(const Config& cfg, int sample_rate, int channels,
                      std::string& error) {
    close();
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

    // The AES67 card is not played the way a sound card is. Its daemon reads a
    // calendar of one-millisecond slots on a clock of its own, so the audio is
    // addressed into that calendar instead — see digisyn_calendar.h, and
    // scripts/player/digisyn_probe.c which proved the write on the bench. The
    // card is identified by its own name rather than by the config string, so
    // every way of naming it (hw:, plughw:, sysdefault:, default) takes this
    // branch and every other device, HDMI included, is left exactly as it was.
    //
    // The answer is logged either way. The first version of this check asked the
    // PCM's *name* for "Digisyn", which the vendor sets to "Dummy PCM", so it
    // never matched and the calendar was silently skipped — a whole bench
    // session of "broken up" with no calendar line anywhere in it. Saying which
    // answer matched, and saying so when none did, is the fix for that.
    const char* why = digisyn_reason(m_pcm, device);
    if (why != nullptr) {
        plog_info("%s is the AES67 card (%s) — using its calendar, not ALSA",
                  device.c_str(), why);
        if (open_calendar(channels, error)) {
            // The PCM is not written to in this mode; let it go so nothing else
            // believes the card is held open through ALSA.
            snd_pcm_close(m_pcm);
            m_pcm = nullptr;
            return true;
        }
        // Fall through rather than go silent: broken-up audio beats none, and
        // the reason is in the log.
        plog_warn("the AES67 card's calendar could not be used (%s) — falling "
                  "back to ALSA, which its driver cannot keep up with",
                  error.c_str());
        error.clear();
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

    m_source_channels = channels;
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
    // be made larger. Nothing said so, and the consequence is not obvious: the
    // delivery thread also presents the picture, so any stall longer than the
    // buffer under-runs the card. A box that reports "broken up" thousands of
    // times is usually being told this, not that its power supply is weak.
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
                      "(%.0f ms of it per period, %d periods). The thread that "
                      "writes audio also presents the picture, so anything that "
                      "stalls it longer than that gaps the sound.",
                      device.c_str(), granted_ms, asked_ms, period_ms,
                      (int)(buffer_size / (period_size ? period_size : 1)));
        }
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
    return true;
}

// ── The AES67 calendar ──────────────────────────────────────────────────────
//
// None of this goes through ALSA. The daemon reads a calendar of
// one-millisecond slots on a clock of its own and transmits whatever it finds,
// so the audio is addressed into that calendar directly. The bench proved the
// write (scripts/player/digisyn_probe.c put a clean 1 kHz tone out of the
// network this way), which is why the PCM is closed rather than merely unused.

bool AlsaOutput::open_calendar(int feed_channels, std::string& error) {
    const int fd = ::open(kDigisynDevice, O_RDWR);
    if (fd < 0) {
        error = std::string("cannot open ") + kDigisynDevice + ": " +
                std::strerror(errno);
        return false;
    }

    // The header is in the first page and states how big the whole mapping is.
    // The size is read, never assumed: it depends on the module's build and on
    // the kernel's page size, and assuming 4K once rejected a device that was
    // perfectly fine because the Pi ran 16K pages.
    void* head = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (head == MAP_FAILED) {
        error = std::string("cannot map ") + kDigisynDevice + ": " +
                std::strerror(errno);
        ::close(fd);
        return false;
    }
    const DigisynHeader* h = reinterpret_cast<const DigisynHeader*>(head);
    const size_t map_size = h->mapSize;
    if (h->verifyCodeStart != kDigisynVerifyCode ||
        h->verifyCodeEnd != kDigisynVerifyCode) {
        error = "the AES67 mapping does not carry the vendor's stamp — is "
                "DigiAes67Proc running?";
        ::munmap(head, 4096);
        ::close(fd);
        return false;
    }
    if (map_size < sizeof(DigisynHeader) || map_size > (1u << 24) ||
        h->bufMs == 0 || h->chToNet == 0 || h->sampleRate == 0) {
        error = "the AES67 mapping is not usable (size " +
                std::to_string(map_size) + ", " + std::to_string(h->bufMs) +
                " ms, " + std::to_string(h->chToNet) + " channels)";
        ::munmap(head, 4096);
        ::close(fd);
        return false;
    }
    ::munmap(head, 4096);

    void* whole = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd, 0);
    if (whole == MAP_FAILED) {
        error = "cannot map " + std::to_string(map_size) + " bytes of " +
                kDigisynDevice + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    ::close(fd);   // the mapping outlives the descriptor

    m_map = whole;
    m_map_size = map_size;
    m_hdr = reinterpret_cast<DigisynHeader*>(whole);

    // The calendar takes 32-bit integers at the daemon's rate and channel
    // count, whatever the stream carries, so a decoded frame is converted into
    // that shape here — the same conversion the ALSA path does for an S32 card.
    m_rate = (int)m_hdr->sampleRate;
    m_channels = (int)m_hdr->chToNet;
    m_format = PcmOutFormat::S32;
    m_slot_bytes = (size_t)digisyn_slot_samples(*m_hdr) * sizeof(int32_t);
    m_slot_buf.assign(m_slot_bytes, 0);

    const size_t frame_bytes = (size_t)m_channels * sizeof(int32_t);
    m_queue.reset(frame_bytes * (size_t)m_rate / 1000 * (size_t)kDigisynQueueMs);
    m_starved.store(0);
    m_logged_starved.store(0);
    m_dropped.store(0);
    m_next_ms.store(0);
    m_stop.store(false);

    m_description = "AES67 calendar, " + std::to_string(m_channels) +
                    (m_channels == 1 ? " channel at " : " channels at ") +
                    std::to_string(m_rate) + " Hz, S32, " +
                    std::to_string(kDigisynQueueMs) + " ms queue";

    // Padding the feed's channels with silence is the same policy the ALSA path
    // took: play what the card will take, and say so rather than fail.
    if (feed_channels > 0 && feed_channels < m_channels) {
        plog_warn("%s takes %d channels but the feed carries %d — the extra "
                  "channels are silent. Check the output device, or use an "
                  "HDMI de-embedder that takes all %d.",
                  kDigisynDevice, m_channels, feed_channels, m_channels);
    }

    m_calendar.store(true);
    m_feeder_started.store(true);
    m_feeder = std::thread(&AlsaOutput::feed, this);
    return true;
}

// Stops the feeder, then lets go of the card. Reachable from the delivery loop
// and from the interface (an operator switching audio off), so it is written so
// that two callers cannot both join the feeder: the join is done under m_mtx,
// which serialises the callers, and the feeder never takes m_mtx — it takes only
// m_queue_mtx — so holding it across the join cannot deadlock. The feeder sleeps
// between slots, so the join returns within a millisecond.
void AlsaOutput::close() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_stop.store(true, std::memory_order_release);
    m_calendar.store(false, std::memory_order_release);
    if (m_feeder.joinable()) m_feeder.join();
    m_feeder_started.store(false, std::memory_order_release);

    if (m_map) {
        ::munmap(m_map, m_map_size);
        m_map = nullptr;
        m_map_size = 0;
        m_hdr = nullptr;
    }
    {
        std::lock_guard<std::mutex> qlk(m_queue_mtx);
        m_queue.clear();
    }
    m_next_ms.store(0, std::memory_order_relaxed);

    if (m_pcm) {
        snd_pcm_drop(m_pcm);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
    }
    m_description = "no audio output";
}

bool AlsaOutput::recover(int err) {
    // An under-run means the box did not keep up — worth counting, because it
    // is the audible symptom of a machine that is thermally throttled or doing
    // too much. Recovery is silent; the count is not.
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
              "this is not the hardware. If the card's buffer is smaller than "
              "a decoded frame (the log says so as it opens), suspect that "
              "first; otherwise it is the feed or a burst of seeking";
        plog_warn("sound has broken up %lld times%s", m_xruns, why);
    }
    return true;
}

void AlsaOutput::write(const multisite::DecodedAudioFrame& frame) {
    if (frame.frames == 0 || frame.interleaved.empty()) return;

    // The calendar path first, and without taking the lock. This is the thread
    // that presents the picture; a feeder that is mid-write must never be able
    // to stall it, and a memcpy into a queue is short enough that it does not.
    if (m_calendar.load(std::memory_order_acquire)) {
        write_calendar(frame);
        return;
    }

    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm || frame.frames == 0 || frame.interleaved.empty()) return;

    // Straight through only when the card took float *and* the feed's channel
    // count already matches the device: the decoder's own buffer is then
    // exactly what ALSA is waiting for, and copying every sample on the thread
    // that also presents the picture is worth avoiding.
    if (m_format == PcmOutFormat::Float32 && frame.channels == m_channels) {
        write_frames(reinterpret_cast<const uint8_t*>(frame.interleaved.data()),
                     frame.frames);
        return;
    }

    // Otherwise convert: to integers, or to the channel count the card would
    // take, or both. The policy is the one the float path already used — play
    // what the card will take rather than nothing at all, having said so in the
    // log when whole channels are being dropped.
    pcm_convert(frame.interleaved.data(), frame.frames, frame.channels,
                m_channels, m_format, m_bytes);
    const size_t frame_bytes = (size_t)m_channels *
                               (size_t)pcm_bytes_per_sample(m_format);
    if (frame_bytes > 0)
        write_frames(m_bytes.data(), m_bytes.size() / frame_bytes);
}

void AlsaOutput::write_calendar(const multisite::DecodedAudioFrame& frame) {
    // Taken under the lock only so close() cannot unmap the calendar out from
    // under this conversion. It is not held long — a convert and a memcpy — and
    // the feeder never takes this lock, so nothing the feeder does can stall the
    // thread that presents the picture through here.
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_calendar.load(std::memory_order_relaxed) || !m_hdr) return;

    // The calendar takes 32-bit integers, and the daemon it feeds was built for
    // a fixed channel count. The decoder's floats are converted once, here, on
    // the way in — not on the feeder, which has one millisecond to meet.
    pcm_convert(frame.interleaved.data(), frame.frames, frame.channels,
                (int)m_hdr->chToNet, PcmOutFormat::S32, m_bytes);
    if (m_bytes.empty()) return;

    queue_bytes(m_bytes.data(), m_bytes.size());
}

// Hands bytes to the queue the feeder drains. Never blocks on the feeder: a
// full queue drops its oldest audio and counts it, because the alternative —
// waiting for room — would stall the thread that presents the picture, which is
// the whole fault this path exists to avoid.
void AlsaOutput::queue_bytes(const uint8_t* data, size_t n) {
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m_queue_mtx);
    const size_t pushed = m_queue.push(data, n);
    if (pushed < n) m_dropped.fetch_add((long long)(n - pushed));
}

// Fills one millisecond of the calendar: a millisecond of audio if the queue
// has it, silence if it does not. A starved slot is counted rather than skipped
// — a gap in the sound is the symptom, and the count is what says how often.
// Feeder thread only, so it takes no lock on the mapping itself.
void AlsaOutput::write_slot(const DigisynHeader& h, uint64_t ms_index) {
    size_t got = 0;
    {
        std::lock_guard<std::mutex> lk(m_queue_mtx);
        got = m_queue.pop(m_slot_buf.data(), m_slot_bytes);
    }
    if (got < m_slot_bytes) {
        std::memset(m_slot_buf.data() + got, 0, m_slot_bytes - got);
        m_starved.fetch_add(1);
    }
    std::memcpy(reinterpret_cast<char*>(m_hdr) + digisyn_slot_offset(h, ms_index),
                m_slot_buf.data(), m_slot_bytes);
}

// The feeder thread. Its whole job is to keep the next few milliseconds of the
// calendar filled, at the daemon's own clock rather than at the rate frames
// arrive, so the daemon always finds audio in the slot for the millisecond it
// has reached. It sleeps between slots; it never spins, and it never touches
// ALSA, which is what let this replace a design that under-ran thirty times a
// second on a card whose driver cannot recover from an under-run at all.
void AlsaOutput::feed() {
    using namespace std::chrono;

    // Let the queue fill a little before the first slot goes out, so a fresh
    // start is not a moment of silence — but not for long: a slow feed should
    // still be heard, starved slots and all.
    const size_t primed_target = m_slot_bytes * (size_t)kDigisynLeadMs;
    for (int i = 0; i < 200 && !m_stop.load(std::memory_order_acquire); ++i) {
        size_t held = 0;
        {
            std::lock_guard<std::mutex> lk(m_queue_mtx);
            held = m_queue.size();
        }
        if (held >= primed_target) break;
        std::this_thread::sleep_for(milliseconds(5));
    }

    while (!m_stop.load(std::memory_order_acquire)) {
        if (!m_hdr) {
            std::this_thread::sleep_for(milliseconds(2));
            continue;
        }
        const DigisynHeader& h = *m_hdr;
        const uint64_t now = h.msIndex;
        if (now == 0) {   // the daemon has not started its clock yet
            std::this_thread::sleep_for(microseconds(500));
            continue;
        }

        // Where to aim, held in m_next_ms rather than a local so flush() can
        // reset it after a seek. Zero means "not started" — a fresh start and a
        // seek both re-prime here, which is what stops the first slot after a
        // jump being audio from where playback used to be.
        uint64_t next = m_next_ms.load(std::memory_order_acquire);
        if (next == 0) next = now + kDigisynLeadMs;

        // If the clock has moved on — started late, or a stall long enough that
        // the target is now in the past or further ahead than the calendar is
        // deep — resync rather than write a slot the daemon has already
        // transmitted, or will not reach for a whole calendar's worth of time.
        if (!digisyn_slot_is_ahead(h, now, next) ||
            next > now + h.bufMs) {
            next = now + kDigisynLeadMs;
        }

        if (next <= now + kDigisynLeadMs) {
            write_slot(h, next);
            ++next;
            m_next_ms.store(next, std::memory_order_release);
        } else {
            m_next_ms.store(next, std::memory_order_release);
            // Ahead of the lead window: rest. Re-read the clock often, so a
            // stop is noticed promptly and the lead does not drift.
            std::this_thread::sleep_for(microseconds(500));
        }
    }

    m_logged_starved.store(m_starved.load(std::memory_order_relaxed));
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
    // On the calendar path there is no PCM to ask. What is written but not yet
    // heard is what the queue holds, plus the lead the feeder keeps in front of
    // the daemon's clock — the same "seconds behind" the ALSA path reports, so
    // the playout clock's drift watch sees a number either way.
    if (m_calendar.load(std::memory_order_acquire)) {
        size_t held = 0;
        {
            std::lock_guard<std::mutex> qlk(m_queue_mtx);
            held = m_queue.size();
        }
        const double rate = m_rate > 0 ? (double)m_rate : 48000.0;
        const double chans = m_channels > 0 ? (double)m_channels : 1.0;
        return (double)held / (rate * chans * 4.0) +
               (double)kDigisynLeadMs / 1000.0;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return 0.0;
    snd_pcm_sframes_t frames = 0;
    if (snd_pcm_delay(m_pcm, &frames) < 0 || frames < 0) return 0.0;
    return (double)frames / (double)m_rate;
}

void AlsaOutput::flush() {
    // After a jump, whatever is queued belongs to where playback used to be.
    // The calendar path is handled without m_mtx (which the ALSA path needs for
    // the PCM): the queue has its own lock, and m_next_ms is atomic because the
    // feeder reads it. Setting it to zero tells the feeder to re-prime from the
    // daemon's clock, so the first slot after a seek is not stale audio.
    if (m_calendar.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> qlk(m_queue_mtx);
            m_queue.clear();
        }
        m_next_ms.store(0, std::memory_order_release);
        return;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return;
    // After a jump, whatever is buffered belongs to where playback used to be.
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
