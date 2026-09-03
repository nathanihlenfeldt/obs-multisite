#pragma once
//
// broadcast_controller.h — owns the encoder-side broadcast.
//
// OBS does not expose custom outputs in its UI, so something has to create the
// output and its encoders and start them. That was a Lua script; this brings it
// into the plugin so the dock can drive it directly.
//
// Deliberately free of Qt: the dock is a thin view over this, and the same
// controller is reachable from hotkeys or (later) obs-websocket.
//
#include <obs.h>

#include <string>
#include <vector>

namespace multisite_obs {

struct BroadcastSettings {
    // storage
    std::string endpoint_host;      // blank when using an R2 account id
    std::string r2_account_id;
    std::string bucket;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "auto";
    std::string room_id = "main-auditorium";
    bool        use_object_tags = false;   // R2 rejects tagging

    // media
    double segment_duration_s = 6.0;
    int    video_bitrate_kbps = 6000;
    int    audio_bitrate_kbps = 160;
    int    audio_tracks = 1;               // OBS mixer tracks to send
    std::string track_labels =
        "Main mix,Sermon ISO,Click";
    std::string channel_labels =
        "Main L,Main R,Sermon ISO,Click,Spare 5,Spare 6,Spare 7,Spare 8";
    std::string marker_labels =
        "Sermon Start,Offering,Go to local,Dismissal";

    // Persisted alongside OBS's own plugin config.
    void load();
    void save() const;
};

// Live status, polled by the dock for display.
struct BroadcastStatus {
    bool        live = false;
    std::string event_id;
    uint64_t    confirmed = 0;
    size_t      pending = 0;
    uint64_t    retries = 0;
    uint64_t    bytes = 0;
    int         link_health = 0;      // 0 healthy, 1 degraded, 2 offline
    std::string last_error;
    double      uptime_s = 0.0;
};

class BroadcastController {
public:
    static BroadcastController& instance();

    const BroadcastSettings& settings() const { return m_cfg; }
    void set_settings(const BroadcastSettings& s);

    // Creates the output plus a video encoder and one audio encoder per
    // requested track, then starts it. Returns false and fills `error` on
    // failure — the dock shows that rather than the operator hunting the log.
    bool go_live(std::string& error);
    void end_broadcast();

    bool is_live() const;
    BroadcastStatus status() const;

    // Drop a marker on the running broadcast.
    void drop_marker(const std::string& label);

private:
    BroadcastController() = default;
    void release_all();

    BroadcastSettings m_cfg;
    obs_output_t*  m_output = nullptr;
    obs_encoder_t* m_venc = nullptr;
    std::vector<obs_encoder_t*> m_aencs;
    uint64_t m_started_ns = 0;
};

} // namespace multisite_obs
