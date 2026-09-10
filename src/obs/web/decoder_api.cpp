// SPDX-License-Identifier: GPL-3.0-or-later
#include "decoder_api.h"
#include "api_common.h"
#include "web_ui.h"

#include "../decoder_settings.h"
#include "../multisite_ui.h"
#include "../plugin_log.h"
#include "../plugin_role.h"

#include <obs-module.h>

#include <functional>
#include <string>

namespace multisite_obs {

namespace {

// One document, polled twice a second, and the page draws everything from it.
// The field names follow the campus player's own status document, so an
// operator who has used that page reads this one without learning it again.
json decoder_status() {
    DecoderSnapshot s;
    const bool have_source = decoder_snapshot(s);

    json j;
    j["role"]    = "decoder";
    j["now_ms"]  = now_ms();
    j["version"] = PLUGIN_VERSION;
    // The remote surface's own lock, and the one the desk set. Both are shown,
    // because "why will this not respond" has two different answers.
    j["locked"]       = web_ui_locked();
    j["source_locked"] = s.locked;
    // The other half of the plugin, when this machine has one.
    j["other_page"] = plugin_role() == Role::DecoderOnly ? "" : "/encoder/";

    // No source means no picture and nothing to control, and it must not look
    // like a player that has merely stopped: the fix is in the scene collection,
    // not on this page.
    j["have_source"] = have_source;
    if (!have_source) return j;

    j["room_id"]   = s.room_id;
    j["room_state"] = s.room_state;
    j["event_id"]  = s.event_id;
    j["pinned_event_id"] = s.pinned_event_id;
    j["live_elsewhere"]  = s.live_elsewhere;
    j["live_event_id"]   = s.live_event_id;

    j["playing"]    = s.playing;
    j["paused"]     = s.paused;
    j["buffering"]  = s.buffering;
    j["loading"]    = s.loading;
    j["ended"]      = s.ended;
    j["at_end"]     = s.at_end;
    j["was_live"]   = s.was_live;
    j["interrupted"] = s.interrupted;

    j["playhead_ms"]    = s.playhead_ms;
    j["live_ms"]        = s.live_ms;
    j["earliest_ms"]    = s.earliest_ms;
    j["started_ms"]     = s.started_ms;
    j["end_ms"]         = s.end_ms;
    j["total_ms"]       = s.total_ms;
    j["seek_target_ms"] = s.seek_target_ms;
    j["behind_live_s"]  = s.behind_live_s;
    j["buffered_ahead_s"] = s.buffered_ahead_s;
    j["cached_segments"]  = (unsigned long long)s.cached;

    j["link_health"] = s.link_health;
    j["link_known"]  = s.link_known;
    j["last_error"]  = s.last_error;
    j["colo"]        = s.colo;
    j["storage_host"] = s.storage_host;
    j["download_bytes_per_s"] = s.download_bytes_per_s;
    j["download_samples"]     = (unsigned long long)s.download_samples;

    j["audio_channels"]   = s.audio_channels;
    j["audio_track_label"] = s.audio_track_label;
    j["current_marker"]   = s.current_marker;

    json channels = json::array();
    for (const auto& c : s.channel_labels) channels.push_back(c);
    j["channel_labels"] = std::move(channels);

    json spans = json::array();
    for (const auto& sp : s.cached_spans)
        spans.push_back(json{{"from_ms", sp.first}, {"to_ms", sp.second}});
    j["cached_spans"] = std::move(spans);

    json markers = json::array();
    for (const auto& m : s.markers)
        markers.push_back(json{{"label", m.label}, {"id", m.id},
                               {"at_ms", m.at_ms}});
    j["markers"] = std::move(markers);

    const DecoderSettings& cfg = decoder_settings();
    j["configured"] = cfg.configured();
    return j;
}

json decoder_events_json() {
    EventListing l;
    decoder_event_listing(l);

    json j;
    j["loading"]       = l.loading;
    j["listed_once"]   = l.listed_once;
    j["fallback_scan"] = l.fallback_scan;
    j["skipped"]       = l.skipped;
    j["error"]         = l.error;

    json rows = json::array();
    for (const auto& e : l.events)
        rows.push_back(json{{"event_id", e.event_id}, {"name", e.name},
                            {"started_ms", e.started_ms},
                            {"duration_s", e.duration_s}, {"state", e.state}});
    j["events"] = std::move(rows);
    return j;
}

json decoder_settings_json(const DecoderSettings& s) {
    json j;
    j["endpoint_host"] = s.endpoint_host;
    j["r2_account_id"] = s.r2_account_id;
    j["bucket"]        = s.bucket;
    j["access_key_id"] = s.access_key_id;
    // Never the secret itself: the placeholder coming back means "unchanged".
    j["secret_access_key"] = s.secret_access_key.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["region"]               = s.region;
    j["room_id"]              = s.room_id;
    j["prebuffer_segments"]   = s.prebuffer_segments;
    j["start_buffer_seconds"] = s.start_buffer_seconds;
    j["poll_interval_ms"]     = s.poll_interval_ms;
    j["keep_behind_segments"] = s.keep_behind_segments;
    j["buffer_minutes"]       = s.buffer_minutes;
    return j;
}

// Guard rails rather than trust: a figure typed on a phone in a dark room must
// not be able to stop this campus playing, from the page being used to fix it.
void apply_settings(const json& j, DecoderSettings& s) {
    json_str(j, "endpoint_host", s.endpoint_host);
    json_str(j, "r2_account_id", s.r2_account_id);
    json_str(j, "bucket",        s.bucket);
    json_str(j, "access_key_id", s.access_key_id);
    json_str(j, "region",        s.region);
    json_str(j, "room_id",       s.room_id);

    std::string secret;
    if (json_str(j, "secret_access_key", secret) && secret != kSecretPlaceholder)
        s.secret_access_key = secret;

    int n;
    if (json_int(j, "prebuffer_segments", n))   s.prebuffer_segments = n;
    if (json_int(j, "start_buffer_seconds", n)) s.start_buffer_seconds = n;
    if (json_int(j, "poll_interval_ms", n))     s.poll_interval_ms = n;
    if (json_int(j, "keep_behind_segments", n)) s.keep_behind_segments = n;
    if (json_int(j, "buffer_minutes", n))       s.buffer_minutes = n;

    if (s.prebuffer_segments   < 0)    s.prebuffer_segments = 0;
    if (s.start_buffer_seconds < 0)    s.start_buffer_seconds = 0;
    if (s.poll_interval_ms     < 500)  s.poll_interval_ms = 500;
    if (s.keep_behind_segments < 10)   s.keep_behind_segments = 10;
    if (s.buffer_minutes       < 1)    s.buffer_minutes = 1;
    if (s.buffer_minutes       > 240)  s.buffer_minutes = 240;
    if (s.room_id.empty())             s.room_id = "main-auditorium";
}

using ControlAction = std::function<void(const multisite::HttpRequest&)>;

// A control: refused while locked, and answered with the new status.
//
// Unlike the encoder's, this needs no thread hand-off: these are the same
// functions the hotkeys call, which were written to be reached from a thread
// nobody owns.
multisite::HttpHandler decoder_control(ControlAction act) {
    return [act](const multisite::HttpRequest& req,
                 multisite::HttpResponse& res) {
        if (web_ui_locked()) {
            res.status = 409;
            res.json(json{{"error", "the controls are locked"},
                          {"locked", true}}.dump());
            return;
        }
        act(req);
        res.json(decoder_status().dump());
    };
}

} // namespace

void register_decoder_api(multisite::HttpServer& server) {
    using multisite::HttpRequest;
    using multisite::HttpResponse;

    server.route("GET", "/api/decoder/status",
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(decoder_status().dump());
    });

    // ── Transport ───────────────────────────────────────────────────────────
    // Named for what an operator says, not for what the pipeline does: loading
    // buffers, playing goes to air, holding keeps the picture on screen.
    server.route("POST", "/api/decoder/play", decoder_control(
        [](const HttpRequest&) { decoder_play_all(); }));
    server.route("POST", "/api/decoder/stop", decoder_control(
        [](const HttpRequest&) { decoder_stop_all(); }));
    server.route("POST", "/api/decoder/hold", decoder_control(
        [](const HttpRequest&) { decoder_pause_all(); }));
    server.route("POST", "/api/decoder/continue", decoder_control(
        [](const HttpRequest&) { decoder_resume_all(); }));
    server.route("POST", "/api/decoder/catch-up", decoder_control(
        [](const HttpRequest&) { decoder_jump_live_all(); }));

    server.route("POST", "/api/decoder/jog", decoder_control(
        [](const HttpRequest& req) {
            decoder_jog(num_param(req, "seconds", 0.0));
        }));

    server.route("POST", "/api/decoder/seek", decoder_control(
        [](const HttpRequest& req) {
            decoder_seek_time((long long)num_param(req, "ms", 0.0));
        }));

    server.route("POST", "/api/decoder/delay", decoder_control(
        [](const HttpRequest& req) {
            decoder_set_delay(num_param(req, "seconds", 0.0));
        }));

    server.route("POST", "/api/decoder/marker", decoder_control(
        [](const HttpRequest& req) {
            const std::string id = str_param(req, "id");
            if (!id.empty()) decoder_jump_to_marker(id);
        }));

    // ── Recordings ──────────────────────────────────────────────────────────
    server.route("GET", "/api/decoder/events",
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(decoder_events_json().dump());
    });

    // Refreshing is a request, not a wait: listing a bucket plus one manifest
    // per event is far too slow to make a phone stand still for, so the answer
    // is the listing as it stands and the next poll shows it filling in.
    server.route("POST", "/api/decoder/events/refresh", decoder_control(
        [](const HttpRequest&) { decoder_refresh_events(); }));

    server.route("POST", "/api/decoder/load-event", decoder_control(
        [](const HttpRequest& req) {
            const json j = body_object(req);
            std::string id;
            if (!json_str(j, "event_id", id)) id = str_param(req, "event_id");
            if (!id.empty()) decoder_pin_event(id);
        }));

    server.route("POST", "/api/decoder/return-to-live", decoder_control(
        [](const HttpRequest&) { decoder_unpin_event(); }));

    // ── Settings ────────────────────────────────────────────────────────────
    server.route("GET", "/api/decoder/settings",
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(decoder_settings_json(decoder_settings_copy()).dump());
    });

    server.route("POST", "/api/decoder/settings",
                 [](const HttpRequest& req, HttpResponse& res) {
        if (web_ui_locked()) { fail(res, 409, "the controls are locked"); return; }

        DecoderSettings s = decoder_settings_copy();
        apply_settings(body_object(req), s);
        set_decoder_settings(s);
        // The sources are already running with the old figures: they re-read
        // them, exactly as saving in the dock does.
        decoder_reconfigure_all();
        res.json(decoder_settings_json(decoder_settings_copy()).dump());
    });
}

} // namespace multisite_obs
