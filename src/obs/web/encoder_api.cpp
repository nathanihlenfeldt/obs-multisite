// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoder_api.h"
#include "api_common.h"
#include "ui_thread.h"
#include "web_ui.h"

#include "../broadcast_controller.h"
#include "../plugin_log.h"
#include "../plugin_role.h"

#include <obs-module.h>

#include <ctime>
#include <functional>
#include <memory>
#include <string>

namespace multisite_obs {

namespace {

// The name the event-name field starts with, so a field nobody touched still
// names "now". The same rule the dock follows, expressed without Qt: this file
// runs on a network thread and has no business reaching for a widget toolkit.
std::string default_event_name() {
    const std::time_t t = std::time(nullptr);
    char buf[64];
    if (!std::strftime(buf, sizeof(buf), "%a %d %b %Y, %H:%M",
                       std::localtime(&t)))
        return "";
    return buf;
}

// One document, polled twice a second, and the page draws everything from it —
// so no two parts of the interface can disagree about what is happening.
json encoder_status() {
    BroadcastController& c = BroadcastController::instance();
    const BroadcastSettings cfg = c.settings_copy();
    const BroadcastStatus   st  = c.status();

    json j;
    j["role"]    = "encoder";
    j["now_ms"]  = now_ms();
    j["version"] = PLUGIN_VERSION;
    j["locked"]  = web_ui_locked();

    j["live"]       = st.live;
    j["event_id"]   = st.event_id;
    j["event_name"] = cfg.event_name;
    j["uptime_s"]   = st.uptime_s;
    j["confirmed"]  = (unsigned long long)st.confirmed;
    j["pending"]    = (unsigned long long)st.pending;
    j["retries"]    = (unsigned long long)st.retries;
    j["bytes"]      = (unsigned long long)st.bytes;
    j["last_error"] = st.last_error;

    // The internet reading, and whether it is a measurement or a default. A
    // page that said "healthy" before anything had been tried would be telling
    // an operator the thing they most need to know, wrongly.
    j["link_health"]        = st.link_health;
    j["link_known"]         = st.link_known;
    j["colo"]               = st.colo;
    j["storage_host"]       = st.storage_host;
    j["upload_bytes_per_s"] = st.upload_bytes_per_s;
    j["upload_samples"]     = (unsigned long long)st.upload_samples;

    j["room_id"]    = cfg.room_id;
    j["bucket"]     = cfg.bucket;
    j["configured"] = !cfg.bucket.empty() &&
                      (!cfg.endpoint_host.empty() || !cfg.r2_account_id.empty());
    j["video_encoder_id"]   = cfg.video_encoder_id;
    j["default_event_name"] = default_event_name();
    // The other half of the plugin, when this machine has one. A page linking to
    // a route that was never registered is a page that looks broken.
    j["other_page"] = plugin_role() == Role::EncoderOnly ? "" : "/decoder/";

    json markers = json::array();
    for (const auto& m : split_csv(cfg.marker_labels)) markers.push_back(m);
    j["marker_labels"] = std::move(markers);
    return j;
}

json encoder_settings_json(const BroadcastSettings& s) {
    json j;
    j["endpoint_host"] = s.endpoint_host;
    j["r2_account_id"] = s.r2_account_id;
    j["bucket"]        = s.bucket;
    j["access_key_id"] = s.access_key_id;
    // Never the secret itself. Sending the placeholder back means "unchanged",
    // so nobody has to retype a bucket key in order to change a room name — and
    // the key never has to travel to a phone in order to come back again.
    j["secret_access_key"] = s.secret_access_key.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["region"]             = s.region;
    j["room_id"]            = s.room_id;
    j["send_expiry_tag"]    = s.send_expiry_tag;
    j["video_encoder_id"]   = s.video_encoder_id;
    j["segment_duration_s"] = s.segment_duration_s;
    j["video_bitrate_kbps"] = s.video_bitrate_kbps;
    j["audio_bitrate_kbps"] = s.audio_bitrate_kbps;
    j["audio_tracks"]       = s.audio_tracks;
    j["track_labels"]       = s.track_labels;
    j["channel_labels"]     = s.channel_labels;
    j["marker_labels"]      = s.marker_labels;

    // The encoders this machine actually has, so the page offers the same list
    // the dock does rather than a text field to mistype an id into.
    json encoders = json::array();
    for (const auto& e : available_video_encoders())
        encoders.push_back(json{{"id", e.id}, {"name", e.name},
                                {"codec", e.codec}, {"hardware", e.hardware}});
    j["encoders"] = std::move(encoders);
    return j;
}

// Applies a posted document to a copy of the stored settings. The guard rails
// live here rather than in the page: a mistyped figure must not be able to make
// broadcasting impossible from the very interface being used to fix it.
void apply_settings(const json& j, BroadcastSettings& s) {
    json_str(j, "endpoint_host",    s.endpoint_host);
    json_str(j, "r2_account_id",    s.r2_account_id);
    json_str(j, "bucket",           s.bucket);
    json_str(j, "access_key_id",    s.access_key_id);
    json_str(j, "region",           s.region);
    json_str(j, "room_id",          s.room_id);
    json_str(j, "video_encoder_id", s.video_encoder_id);
    json_str(j, "track_labels",     s.track_labels);
    json_str(j, "channel_labels",   s.channel_labels);
    json_str(j, "marker_labels",    s.marker_labels);
    json_bool(j, "send_expiry_tag", s.send_expiry_tag);

    std::string secret;
    if (json_str(j, "secret_access_key", secret) && secret != kSecretPlaceholder)
        s.secret_access_key = secret;

    double d;
    if (json_num(j, "segment_duration_s", d)) s.segment_duration_s = d;

    int n;
    if (json_int(j, "video_bitrate_kbps", n)) s.video_bitrate_kbps = n;
    if (json_int(j, "audio_bitrate_kbps", n)) s.audio_bitrate_kbps = n;
    if (json_int(j, "audio_tracks", n))       s.audio_tracks = n;

    if (s.segment_duration_s < 1.0)   s.segment_duration_s  = 1.0;
    if (s.segment_duration_s > 30.0)  s.segment_duration_s  = 30.0;
    if (s.video_bitrate_kbps < 200)   s.video_bitrate_kbps  = 200;
    if (s.video_bitrate_kbps > 100000) s.video_bitrate_kbps = 100000;
    if (s.audio_bitrate_kbps < 32)    s.audio_bitrate_kbps  = 32;
    if (s.audio_bitrate_kbps > 320)   s.audio_bitrate_kbps  = 320;
    if (s.audio_tracks < 1)           s.audio_tracks        = 1;
    if (s.audio_tracks > 6)           s.audio_tracks        = 6;
    if (s.room_id.empty())            s.room_id = "main-auditorium";
}

// ── Routes ───────────────────────────────────────────────────────────────────

using ControlAction =
    std::function<void(const multisite::HttpRequest&, std::string&)>;

// A control: refused while locked, run on OBS's UI thread, and answered with the
// new status so the page never has to guess what its own click did.
multisite::HttpHandler encoder_control(ControlAction act) {
    return [act](const multisite::HttpRequest& req,
                 multisite::HttpResponse& res) {
        if (web_ui_locked()) {
            res.status = 409;
            res.json(json{{"error", "the controls are locked"},
                          {"locked", true}}.dump());
            return;
        }

        // Copied rather than referenced: if OBS does not get to the task inside
        // the timeout, this request has already returned and the task may still
        // run later, so it must not be holding anything belonging to a handler
        // that has gone.
        auto error = std::make_shared<std::string>();
        const auto query = req.query;
        const auto body  = req.body;

        if (!run_on_ui_thread([act, error, query, body] {
                multisite::HttpRequest copy;
                copy.query = query;
                copy.body  = body;
                act(copy, *error);
            })) {
            fail(res, 503, "OBS is busy - try again in a moment");
            return;
        }
        if (!error->empty()) { fail(res, 409, *error); return; }
        res.json(encoder_status().dump());
    };
}

} // namespace

void register_encoder_api(multisite::HttpServer& server) {
    using multisite::HttpRequest;
    using multisite::HttpResponse;

    server.route("GET", "/api/encoder/status",
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(encoder_status().dump());
    });

    server.route("POST", "/api/encoder/go-live", encoder_control(
        [](const HttpRequest& req, std::string& error) {
            auto& c = BroadcastController::instance();
            BroadcastSettings s = c.settings_copy();

            // The title arrives in the body or the query, whichever the page
            // found easier. Blank means "name it now", which is what the dock
            // does with a field nobody touched.
            const json j = body_object(req);
            std::string name;
            if (!json_str(j, "event_name", name)) name = str_param(req, "name");
            s.event_name = name.empty() ? default_event_name() : name;
            c.set_settings(s);

            if (!c.go_live(error))
                mlog_warn("go live from the remote control failed: %s",
                          error.c_str());
        }));

    server.route("POST", "/api/encoder/end", encoder_control(
        [](const HttpRequest&, std::string& error) {
            auto& c = BroadcastController::instance();
            if (!c.is_live()) { error = "not broadcasting"; return; }
            c.end_broadcast();
        }));

    server.route("POST", "/api/encoder/marker", encoder_control(
        [](const HttpRequest& req, std::string& error) {
            const std::string label = str_param(req, "label");
            if (label.empty()) { error = "which marker?"; return; }
            BroadcastController::instance().drop_marker(label);
        }));

    server.route("GET", "/api/encoder/settings",
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(encoder_settings_json(
            BroadcastController::instance().settings_copy()).dump());
    });

    server.route("POST", "/api/encoder/settings",
                 [](const HttpRequest& req, HttpResponse& res) {
        if (web_ui_locked()) { fail(res, 409, "the controls are locked"); return; }

        const json in = body_object(req);
        if (!run_on_ui_thread([in] {
                auto& c = BroadcastController::instance();
                BroadcastSettings s = c.settings_copy();
                // The event name is per-event and belongs to Go Live: a settings
                // save must not quietly rename the broadcast that is running.
                const std::string keep = s.event_name;
                apply_settings(in, s);
                s.event_name = keep;
                c.set_settings(s);
            }, 10000)) {
            fail(res, 503, "OBS is busy - try again in a moment");
            return;
        }
        res.json(encoder_settings_json(
            BroadcastController::instance().settings_copy()).dump());
    });
}

} // namespace multisite_obs
