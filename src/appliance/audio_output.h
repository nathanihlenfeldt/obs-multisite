#pragma once
//
// audio_output.h — where the sound goes.
//
// The low-cost tier carries production audio over HDMI: up to eight channels
// of LPCM, recovered at the campus with a de-embedder. That is what makes a
// packed multi-channel feed — main mix, ISOs, click — viable on a box costing
// less than a microphone.
//
// The output is opened with the channel count the FEED carries, not a fixed
// stereo pair, because dropping channels here would silently destroy the
// thing the whole audio design exists to deliver.
//
#include "config.h"
#include "../core/cmaf_decoder.h"

#include <string>
#include <vector>

namespace multisite_player {

struct AudioDevice {
    std::string id;             // what goes in the config, e.g. "hw:CARD=vc4hdmi0"
    std::string description;    // what the operator reads
    int         max_channels = 0;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // Opened once the first decoded audio frame reveals the feed's rate and
    // channel count.
    virtual bool open(const Config& cfg, int sample_rate, int channels,
                      std::string& error) = 0;
    virtual void close() = 0;
    virtual bool ok() const = 0;

    virtual std::string description() const = 0;

    // Interleaved float, exactly as the decoder produced it.
    virtual void write(const multisite::DecodedAudioFrame& frame) = 0;

    // Seconds of audio written but not yet heard. The playout clock watches
    // this: over a two-hour service a soundcard running a few parts per
    // million away from the system clock will drift lips out of sync unless
    // somebody is looking at it.
    virtual double delay_s() const = 0;

    // Discard anything buffered — after a seek, so the sound does not run on
    // from where playback used to be.
    virtual void flush() = 0;

    virtual std::vector<AudioDevice> devices() const = 0;
};

class NullAudioOutput : public AudioOutput {
public:
    bool open(const Config&, int sample_rate, int channels,
              std::string& error) override;
    void close() override {}
    bool ok() const override { return true; }
    std::string description() const override { return m_desc; }
    void write(const multisite::DecodedAudioFrame&) override {}
    double delay_s() const override { return 0.0; }
    void flush() override {}
    std::vector<AudioDevice> devices() const override { return {}; }

private:
    std::string m_desc = "no audio output";
};

#ifdef MULTISITE_HAVE_ALSA
// Returns null when no usable sound card could be opened, so the caller can
// fall back rather than refuse to start.
AudioOutput* make_alsa_output();
#endif

AudioOutput* make_audio_output();

} // namespace multisite_player
