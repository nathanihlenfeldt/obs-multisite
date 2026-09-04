#pragma once
//
// decoder_settings.h — storage credentials for the satellite, stored once per
// machine rather than per source.
//
// They used to live only in each source's own settings, which meant OBS lost
// them if it exited uncleanly, and meant re-entering credentials for every
// additional source (each packed audio track will eventually be its own
// source). Now they are saved alongside OBS's plugin config, and a source
// falls back to them whenever its own fields are blank.
//
#include <string>

namespace multisite_obs {

struct DecoderSettings {
    std::string endpoint_host;      // blank when using an R2 account id
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";
    std::string room_id = "main-auditorium";
    // Kept above zero on purpose: at zero the playhead sits at the live edge,
    // so a hold-and-resume has nothing new to play and the picture appears
    // frozen for up to a segment. A small reserve also absorbs network jitter.
    int    prebuffer_segments = 2;
    int    poll_interval_ms = 3000;
    int    keep_behind_segments = 200;

    void load();
    void save() const;

    bool configured() const {
        return !bucket.empty() &&
               (!endpoint_host.empty() || !r2_account_id.empty());
    }
};

// The machine-wide settings, shared by every multisite source and the dock.
DecoderSettings& decoder_settings();
void set_decoder_settings(const DecoderSettings& s);

} // namespace multisite_obs
