#include "broadcast_controller.h"
#include "plugin_log.h"
#include "multisite_ui.h"

#include <util/platform.h>

#include <algorithm>
#include <vector>

namespace multisite_obs {

// ── Encoder discovery ────────────────────────────────────────────────────────
// Asks OBS what it has rather than assuming: the same plugin runs on machines
// with NVENC, QuickSync, AMF or nothing but x264.
std::vector<EncoderChoice> available_video_encoders() {
    std::vector<EncoderChoice> out;
    const char* id = nullptr;
    for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) {
        if (!id) continue;
        if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO) continue;

        // Skip what OBS itself hides. Enumeration returns legacy aliases and
        // internal entries too, which is why an AMD machine showed the AMF
        // encoder twice: the real one plus a deprecated alias for the same
        // hardware. OBS's own encoder list filters on these flags.
        const uint32_t caps = obs_get_encoder_caps(id);
        if (caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
            continue;

        const char* codec = obs_get_encoder_codec(id);
        if (!codec) continue;
        const std::string c = codec;
        // Only codecs the CMAF muxer and the satellite decoder handle.
        if (c != "h264" && c != "hevc" && c != "av1") continue;

        EncoderChoice e;
        e.id = id;
        const char* disp = obs_encoder_get_display_name(id);
        e.name  = disp ? disp : id;
        e.codec = c;
        // Hardware encoders are identified by name rather than by a flag,
        // because OBS does not expose one.
        e.hardware = (e.id.find("nvenc") != std::string::npos) ||
                     (e.id.find("qsv")   != std::string::npos) ||
                     (e.id.find("amf")   != std::string::npos) ||
                     (e.id.find("_tex")  != std::string::npos) ||
                     (e.id.find("vaapi") != std::string::npos) ||
                     (e.id.find("videotoolbox") != std::string::npos);
        // Belt and braces against two entries that present identically.
        bool dup = false;
        for (const auto& x : out)
            if (x.name == e.name && x.codec == e.codec) { dup = true; break; }
        if (dup) continue;

        out.push_back(e);
    }

    // x264 exists on every OBS install and is the fallback the controller uses,
    // so make sure it is always offered even if enumeration missed it.
    {
        bool have_x264 = false;
        for (const auto& e : out) if (e.id == "obs_x264") have_x264 = true;
        if (!have_x264 && obs_get_encoder_codec("obs_x264")) {
            EncoderChoice e;
            e.id = "obs_x264";
            const char* disp = obs_encoder_get_display_name("obs_x264");
            e.name = disp ? disp : "x264";
            e.codec = "h264";
            out.push_back(e);
        }
    }

    // Hardware first, then by codec, so the best option is the obvious one.
    std::stable_sort(out.begin(), out.end(),
        [](const EncoderChoice& a, const EncoderChoice& b) {
            if (a.hardware != b.hardware) return a.hardware;
            // hevc, then h264, then av1 — av1 last because it has had the
            // least real-world exercise here.
            auto rank = [](const std::string& c) {
                return c == "hevc" ? 0 : (c == "h264" ? 1 : 2);
            };
            return rank(a.codec) < rank(b.codec);
        });
    return out;
}

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
    if (obs_data_has_user_value(d, "video_encoder_id"))
        video_encoder_id = obs_data_get_string(d, "video_encoder_id");
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
    obs_data_set_string(d, "video_encoder_id", video_encoder_id.c_str());

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

    // Video encoder. Two settings are not optional whichever encoder is used:
    //   * the keyframe interval must equal the segment duration, or the muxer
    //     can never cut a segment;
    //   * anything that inserts extra keyframes must be off, or segment
    //     lengths vary (which is what produced 5.6-7.1 MB segments earlier).
    std::string enc_id = m_cfg.video_encoder_id.empty() ? "obs_x264"
                                                        : m_cfg.video_encoder_id;
    // Fall back rather than fail if the chosen encoder has gone (different
    // machine, driver removed, GPU changed).
    {
        bool found = false;
        for (const auto& e : available_video_encoders())
            if (e.id == enc_id) { found = true; break; }
        if (!found) {
            mlog_warn("video encoder '%s' is not available on this machine — "
                      "falling back to x264", enc_id.c_str());
            enc_id = "obs_x264";
        }
    }

    obs_data_t* vs = obs_data_create();
    obs_data_set_int(vs, "bitrate", m_cfg.video_bitrate_kbps);
    obs_data_set_int(vs, "keyint_sec",
                     (int)std::max(1.0, m_cfg.segment_duration_s + 0.5));
    obs_data_set_string(vs, "rate_control", "CBR");

    if (enc_id == "obs_x264") {
        // scenecut inserts IDRs at scene changes, which breaks even spacing.
        obs_data_set_string(vs, "x264opts", "scenecut=0");
    } else if (enc_id.find("nvenc") != std::string::npos) {
        // Look-ahead can move I-frames off the interval, which is the one
        // thing that must not happen: keyframes have to land on the segment
        // boundary or segment lengths vary.
        obs_data_set_bool(vs, "lookahead", false);
    }
    // Nothing else is set. Quality presets differ in key and value between
    // encoder families, and a mistyped key is silently ignored — an earlier
    // attempt set the AMF preset to "quality" and the encoder logged
    // "preset: speed", i.e. it did nothing. OBS's own defaults are sensible,
    // and leaving them alone is more honest than pretending to tune something.
    // Only the settings that the segmenting REQUIRES are forced.

    m_venc = obs_video_encoder_create(enc_id.c_str(), "multisite_v", vs, nullptr);
    obs_data_release(vs);
    if (!m_venc) {
        error = "could not create the video encoder '" + enc_id + "'";
        release_all();
        return false;
    }
    {
        const char* codec = obs_encoder_get_codec(m_venc);
        mlog_info("video encoder: %s (%s), keyframes every %.0fs",
                  enc_id.c_str(), codec ? codec : "?",
                  m_cfg.segment_duration_s);
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
