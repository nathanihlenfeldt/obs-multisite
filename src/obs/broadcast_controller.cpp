#include "broadcast_controller.h"
#include "plugin_log.h"
#include "multisite_ui.h"

#include <util/platform.h>

#include <algorithm>

namespace multisite_obs {

BroadcastController& BroadcastController::instance() {
    static BroadcastController c;
    return c;
}

// ── Settings persistence ─────────────────────────────────────────────────────
// Stored with OBS's own plugin config so it survives restarts and is per-profile
// in the same way OBS's settings are.
void BroadcastSettings::load() {
    char* path = obs_module_config_path("encoder.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;

    endpoint_host      = obs_data_get_string(d, "endpoint_host");
    r2_account_id      = obs_data_get_string(d, "r2_account_id");
    bucket             = obs_data_get_string(d, "bucket");
    access_key_id      = obs_data_get_string(d, "access_key_id");
    secret_access_key  = obs_data_get_string(d, "secret_access_key");
    if (obs_data_has_user_value(d, "region"))
        region = obs_data_get_string(d, "region");
    if (obs_data_has_user_value(d, "room_id"))
        room_id = obs_data_get_string(d, "room_id");
    use_object_tags    = obs_data_get_bool(d, "use_object_tags");
    if (obs_data_has_user_value(d, "segment_duration_s"))
        segment_duration_s = obs_data_get_double(d, "segment_duration_s");
    if (obs_data_has_user_value(d, "video_bitrate_kbps"))
        video_bitrate_kbps = (int)obs_data_get_int(d, "video_bitrate_kbps");
    if (obs_data_has_user_value(d, "audio_bitrate_kbps"))
        audio_bitrate_kbps = (int)obs_data_get_int(d, "audio_bitrate_kbps");
    if (obs_data_has_user_value(d, "audio_tracks"))
        audio_tracks = (int)obs_data_get_int(d, "audio_tracks");
    if (obs_data_has_user_value(d, "track_labels"))
        track_labels = obs_data_get_string(d, "track_labels");
    if (obs_data_has_user_value(d, "channel_labels"))
        channel_labels = obs_data_get_string(d, "channel_labels");
    if (obs_data_has_user_value(d, "marker_labels"))
        marker_labels = obs_data_get_string(d, "marker_labels");
    obs_data_release(d);
}

void BroadcastSettings::save() const {
    char* dir = obs_module_config_path("");
    if (dir) { os_mkdirs(dir); bfree(dir); }

    obs_data_t* d = obs_data_create();
    obs_data_set_string(d, "endpoint_host", endpoint_host.c_str());
    obs_data_set_string(d, "r2_account_id", r2_account_id.c_str());
    obs_data_set_string(d, "bucket", bucket.c_str());
    obs_data_set_string(d, "access_key_id", access_key_id.c_str());
    obs_data_set_string(d, "secret_access_key", secret_access_key.c_str());
    obs_data_set_string(d, "region", region.c_str());
    obs_data_set_string(d, "room_id", room_id.c_str());
    obs_data_set_bool(d, "use_object_tags", use_object_tags);
    obs_data_set_double(d, "segment_duration_s", segment_duration_s);
    obs_data_set_int(d, "video_bitrate_kbps", video_bitrate_kbps);
    obs_data_set_int(d, "audio_bitrate_kbps", audio_bitrate_kbps);
    obs_data_set_int(d, "audio_tracks", audio_tracks);
    obs_data_set_string(d, "track_labels", track_labels.c_str());
    obs_data_set_string(d, "channel_labels", channel_labels.c_str());
    obs_data_set_string(d, "marker_labels", marker_labels.c_str());

    char* path = obs_module_config_path("encoder.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save encoder settings to %s", path);
        bfree(path);
    }
    obs_data_release(d);
}

void BroadcastController::set_settings(const BroadcastSettings& s) {
    m_cfg = s;
    m_cfg.save();
}

bool BroadcastController::is_live() const { return m_output != nullptr; }

// ── Going live ───────────────────────────────────────────────────────────────
bool BroadcastController::go_live(std::string& error) {
    if (m_output) { error = "already broadcasting"; return false; }

    if (m_cfg.bucket.empty()) {
        error = "Bucket is required";
        return false;
    }
    if (m_cfg.endpoint_host.empty() && m_cfg.r2_account_id.empty()) {
        error = "Set either an R2 Account ID or an endpoint host";
        return false;
    }
    if (m_cfg.access_key_id.empty() || m_cfg.secret_access_key.empty()) {
        error = "Access Key ID and Secret Access Key are required";
        return false;
    }

    obs_data_t* s = obs_data_create();
    obs_data_set_string(s, "endpoint_host", m_cfg.endpoint_host.c_str());
    obs_data_set_string(s, "r2_account_id", m_cfg.r2_account_id.c_str());
    obs_data_set_string(s, "bucket", m_cfg.bucket.c_str());
    obs_data_set_string(s, "access_key_id", m_cfg.access_key_id.c_str());
    obs_data_set_string(s, "secret_access_key", m_cfg.secret_access_key.c_str());
    obs_data_set_string(s, "region", m_cfg.region.c_str());
    obs_data_set_string(s, "room_id", m_cfg.room_id.c_str());
    obs_data_set_double(s, "segment_duration_s", m_cfg.segment_duration_s);
    obs_data_set_string(s, "track_labels", m_cfg.track_labels.c_str());
    obs_data_set_string(s, "channel_labels", m_cfg.channel_labels.c_str());
    obs_data_set_string(s, "marker_labels", m_cfg.marker_labels.c_str());
    obs_data_set_bool(s, "use_object_tags", m_cfg.use_object_tags);

    m_output = obs_output_create("multisite_output", "multisite_out", s, nullptr);
    obs_data_release(s);
    if (!m_output) {
        error = "could not create the multisite output (is the plugin loaded?)";
        return false;
    }

    // Video encoder. The keyframe interval MUST be <= the segment duration or
    // the muxer can never cut a segment; scenecut is disabled so keyframes
    // land on the interval and segment lengths stay even.
    obs_data_t* vs = obs_data_create();
    obs_data_set_int(vs, "bitrate", m_cfg.video_bitrate_kbps);
    obs_data_set_int(vs, "keyint_sec",
                     (int)std::max(1.0, m_cfg.segment_duration_s + 0.5));
    obs_data_set_string(vs, "rate_control", "CBR");
    obs_data_set_string(vs, "x264opts", "scenecut=0");
    m_venc = obs_video_encoder_create("obs_x264", "multisite_v", vs, nullptr);
    obs_data_release(vs);
    if (!m_venc) {
        error = "could not create the video encoder";
        release_all();
        return false;
    }
    obs_encoder_set_video(m_venc, obs_get_video());
    obs_output_set_video_encoder(m_output, m_venc);

    // One AAC encoder per requested OBS mixer track.
    const int tracks = std::max(1, std::min(6, m_cfg.audio_tracks));
    for (int i = 0; i < tracks; ++i) {
        obs_data_t* as = obs_data_create();
        obs_data_set_int(as, "bitrate", m_cfg.audio_bitrate_kbps);
        std::string name = "multisite_a" + std::to_string(i + 1);
        obs_encoder_t* enc = obs_audio_encoder_create(
            "ffmpeg_aac", name.c_str(), as, (size_t)i, nullptr);
        obs_data_release(as);
        if (!enc) {
            error = "could not create audio encoder for track " +
                    std::to_string(i + 1);
            release_all();
            return false;
        }
        obs_encoder_set_audio(enc, obs_get_audio());
        obs_output_set_audio_encoder(m_output, enc, (size_t)i);
        m_aencs.push_back(enc);
    }

    if (!obs_output_start(m_output)) {
        const char* le = obs_output_get_last_error(m_output);
        error = le && *le ? le
                          : "the output refused to start — see the OBS log";
        release_all();
        return false;
    }

    m_started_ns = os_gettime_ns();
    mlog_info("broadcast started: room=%s, %d audio track(s), %.1fs segments",
              m_cfg.room_id.c_str(), tracks, m_cfg.segment_duration_s);
    return true;
}

void BroadcastController::end_broadcast() {
    if (!m_output) return;
    mlog_info("ending broadcast (draining the upload queue)");
    obs_output_stop(m_output);
    release_all();
}

void BroadcastController::release_all() {
    for (obs_encoder_t* e : m_aencs) if (e) obs_encoder_release(e);
    m_aencs.clear();
    if (m_venc)   { obs_encoder_release(m_venc); m_venc = nullptr; }
    if (m_output) { obs_output_release(m_output); m_output = nullptr; }
    m_started_ns = 0;
}

BroadcastStatus BroadcastController::status() const {
    BroadcastStatus st;
    st.live = m_output != nullptr;
    if (!st.live) return st;
    st.bytes = obs_output_get_total_bytes(m_output);
    st.uptime_s = m_started_ns
        ? (double)(os_gettime_ns() - m_started_ns) / 1e9 : 0.0;
    // The richer figures (queue depth, retries, link health) live in the
    // output's session; it publishes them through the controls registry.
    EncoderStats es;
    if (encoder_stats(es)) {
        st.event_id    = es.event_id;
        st.confirmed   = es.confirmed;
        st.pending     = (size_t)es.pending;
        st.retries     = es.retries;
        st.link_health = es.link_health;
        st.last_error  = es.last_error;
        if (es.bytes) st.bytes = es.bytes;
    }
    return st;
}

void BroadcastController::drop_marker(const std::string& label) {
    forward_marker_to_encoder(label);
}

} // namespace multisite_obs
