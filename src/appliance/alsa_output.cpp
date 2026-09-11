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

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace multisite_player {

namespace {

class AlsaOutput : public AudioOutput {
public:
    ~AlsaOutput() override { close(); }

    bool open(const Config& cfg, int sample_rate, int channels,
              std::string& error) override;
    void close() override;
    bool ok() const override { return m_pcm != nullptr; }

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

void AlsaOutput::close() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return;
    snd_pcm_drop(m_pcm);
    snd_pcm_close(m_pcm);
    m_pcm = nullptr;
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
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm) return 0.0;
    snd_pcm_sframes_t frames = 0;
    if (snd_pcm_delay(m_pcm, &frames) < 0 || frames < 0) return 0.0;
    return (double)frames / (double)m_rate;
}

void AlsaOutput::flush() {
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
