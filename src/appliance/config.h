#pragma once
//
// config.h — everything the appliance needs to know, in one file on disk.
//
// The appliance has no operator sitting at it, so every setting must be
// reachable from the web UI and must survive a power cut. That makes the
// config file the whole state of the box: storage credentials, which room to
// follow, how the picture and sound leave the machine, and what to show when
// there is nothing to play.
//
// Field names deliberately match the OBS plugin's DecoderSettings, so the two
// halves of the project describe the same things the same way and a setting
// learned in one place means the same in the other.
//
#include <string>
#include <cstdint>

namespace multisite_player {

// What the box puts on screen when nothing is playing. A campus screen is
// visible to a congregation, so "whatever was last decoded" is not always the
// right answer — sometimes black is, and sometimes a holding slide is.
enum class IdleMode {
    Black,      // safest: nothing on the screen
    HoldFrame,  // freeze the last decoded picture (matches the pause behaviour)
    Splash,     // the identity screen: hostname, IP, room, state
    Image,      // a still supplied by the campus (idle_image_path)
};

const char* to_string(IdleMode m);
IdleMode    idle_mode_from_string(const std::string& s, IdleMode fallback);

struct Config {
    // ── Storage (S3-compatible) ──────────────────────────────────────────────
    std::string endpoint_host;          // blank when using an R2 account id
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";

    // ── What to receive ──────────────────────────────────────────────────────
    std::string room_id = "main-auditorium";
    // Play this specific past event instead of following the room. Empty is
    // the normal, live case.
    std::string pinned_event_id;

    // ── Receive tuning (see DecoderConfig for what each one buys) ────────────
    int prebuffer_segments   = 2;
    int poll_interval_ms     = 3000;
    int keep_behind_segments = 200;
    int buffer_minutes       = 10;
    int max_cached_segments  = 2000;
    int stale_after_ms       = 600000;
    // A USB SSD, not the SD card: the cache writes roughly 3 GB an hour and
    // would wear a card out. The install script points this at the SSD if it
    // finds one.
    std::string cache_dir = "/var/lib/multisite-player/cache";

    // ── Video output ─────────────────────────────────────────────────────────
    std::string drm_card;               // blank = first card with a connected output
    std::string connector;              // blank = first connected connector (e.g. "HDMI-A-1")
    // 0 means "whatever the display says it prefers", which is the right
    // default for a screen nobody has measured.
    int  out_width  = 0;
    int  out_height = 0;
    int  out_fps    = 0;

    IdleMode    idle_mode = IdleMode::Splash;
    std::string idle_image_path;

    // ── Audio output ─────────────────────────────────────────────────────────
    bool        audio_enabled = true;
    // ALSA device name. "default" follows the system; the UI lists what the
    // box actually has.
    std::string alsa_device = "default";
    // 0 = take the channel count from the feed. HDMI carries up to 8 channels
    // of LPCM, which is what makes packed multi-channel work on the cheap tier.
    int         audio_channels = 0;

    // ── Control surface ──────────────────────────────────────────────────────
    int         web_port = 8080;
    // Bound to every interface by design: the operator is on a phone on the
    // church network, not on this box.
    std::string web_bind = "0.0.0.0";

    // ── Behaviour on power-up ────────────────────────────────────────────────
    // An appliance that needs someone to press Play after a power cut is not
    // an appliance. Off only for a box being commissioned.
    bool auto_play = true;
    // Sit this far behind live, in seconds. 0 rides the live edge.
    double delay_from_live_s = 0.0;
    // Refuse control changes from the UI until unlocked — the same guard the
    // dock has, for a tablet left on a music stand mid-service.
    bool locked = false;

    bool configured() const {
        return !bucket.empty() &&
               (!endpoint_host.empty() || !r2_account_id.empty());
    }

    // Read from `path`. Missing file is not an error: a freshly installed box
    // has no config and must still boot far enough to show its IP address so
    // somebody can go and give it one.
    bool load(const std::string& path, std::string& error);

    // Written to a temporary file and renamed, so a power cut during a save
    // cannot leave a half-written config that stops the box booting. Mode 0600:
    // it holds the bucket's secret key.
    bool save(const std::string& path, std::string& error) const;
};

// The path the service uses unless --config says otherwise.
const char* default_config_path();

} // namespace multisite_player
