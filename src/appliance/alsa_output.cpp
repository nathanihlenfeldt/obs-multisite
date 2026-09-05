//
// alsa_output.cpp — production audio out of the box.
//
// The whole point of the audio design is that a satellite receives a
// production bus — main mix, mic ISOs, click — not a stereo listener feed. So
// this opens the device with the channel count the FEED carries and refuses to
// quietly downmix: losing the click because a card was opened in stereo would
// destroy the thing the packed multi-channel layout exists to deliver, and it
// would do it silently.
//
// On the low-cost tier the device is HDMI, which carries up to eight channels
// of LPCM; a de-embedder at the campus recovers them.
//
#include "audio_output.h"
#include "log.h"

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

    mutable std::mutex m_mtx;
    snd_pcm_t*  m_pcm = nullptr;
    int         m_rate = 48000;
    int         m_channels = 2;
    std::string m_description = "no audio output";
    // Rate-limit the complaint: a card that keeps under-running must not fill
    // the log faster than it fills its buffer.
    long long   m_xruns = 0;
    long long   m_logged_xruns = 0;
    // Channels the feed carries, when the device would not take them all.
    int         m_source_channels = 0;
    std::vector<float> m_scratch;
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

    // Interleaved float is exactly what the decoder produces, so nothing has
    // to be converted on the way out.
    rc = snd_pcm_hw_params_set_format(m_pcm, hw, SND_PCM_FORMAT_FLOAT_LE);
    if (rc < 0) {
        error = device + " will not take floating-point audio: " + snd_strerror(rc);
        snd_pcm_close(m_pcm);
        m_pcm = nullptr;
        return false;
    }

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
        // a click track and is not will only find out during a service.
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
    // over a two-hour service.
    unsigned buffer_us = 500000;
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

    char desc[200];
    std::snprintf(desc, sizeof(desc), "%s, %d channel%s at %d Hz",
                  device.c_str(), m_channels, m_channels == 1 ? "" : "s",
                  m_rate);
    m_description = desc;
    m_xruns = m_logged_xruns = 0;
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
        plog_warn("sound has broken up %lld times — the box may be running "
                  "hot or short of power", m_xruns);
    }
    return true;
}

void AlsaOutput::write(const multisite::DecodedAudioFrame& frame) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_pcm || frame.frames == 0 || frame.interleaved.empty()) return;

    const float* samples = frame.interleaved.data();
    snd_pcm_uframes_t remaining = frame.frames;

    // The device would not take every channel the feed carries. Play the ones
    // it will rather than nothing at all — the operator has already been told
    // in the log that the rest are missing.
    if (frame.channels != m_channels) {
        m_scratch.resize((size_t)frame.frames * m_channels);
        const int copy = std::min(frame.channels, m_channels);
        for (uint32_t f = 0; f < frame.frames; ++f) {
            const float* in = samples + (size_t)f * frame.channels;
            float* out = m_scratch.data() + (size_t)f * m_channels;
            for (int c = 0; c < copy; ++c) out[c] = in[c];
            for (int c = copy; c < m_channels; ++c) out[c] = 0.0f;
        }
        samples = m_scratch.data();
    }

    while (remaining > 0) {
        const snd_pcm_sframes_t wrote = snd_pcm_writei(m_pcm, samples, remaining);
        if (wrote < 0) {
            if (!recover((int)wrote)) { snd_pcm_close(m_pcm); m_pcm = nullptr; return; }
            continue;
        }
        samples += (size_t)wrote * m_channels;
        remaining -= (snd_pcm_uframes_t)wrote;
    }
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
