//
// outputs.cpp — choosing where picture and sound go.
//
// A box with no display attached, or a development machine with no KMS at all,
// still has to receive the feed and answer its web UI: a campus should be able
// to prepare a service before the screen is plugged in, and a failure to open
// an output must never take the receiver down with it. So the factories fall
// back to a null output and say so, rather than refusing to start.
//
#include "audio_output.h"
#include "video_output.h"
#include "log.h"

namespace multisite_player {

bool NullVideoOutput::open(const Config& cfg, std::string& error) {
    error.clear();
    if (cfg.out_width > 0 && cfg.out_height > 0) {
        m_width  = cfg.out_width;
        m_height = cfg.out_height;
    }
    if (cfg.out_fps > 0) m_fps = cfg.out_fps;
    m_desc = "no display output (this build has no KMS support)";
    return true;
}

bool NullAudioOutput::open(const Config&, int sample_rate, int channels,
                           std::string& error) {
    error.clear();
    m_desc = "no audio output (this build has no ALSA support)";
    (void)sample_rate; (void)channels;
    return true;
}

VideoOutput* make_video_output() {
#ifdef MULTISITE_HAVE_DRM
    if (VideoOutput* drm = make_drm_output()) return drm;
    plog_warn("no usable KMS display — the feed will be received but not "
              "shown. Check that a screen is connected.");
#endif
    return new NullVideoOutput();
}

AudioOutput* make_audio_output() {
#ifdef MULTISITE_HAVE_ALSA
    if (AudioOutput* alsa = make_alsa_output()) return alsa;
    plog_warn("no usable ALSA device — the feed will be received but silent.");
#endif
    return new NullAudioOutput();
}

} // namespace multisite_player
