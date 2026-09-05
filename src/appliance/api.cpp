#include "api.h"
#include "log.h"
#include "sysinfo.h"
#include "video_output.h"
#include "audio_output.h"
#include "preview.h"

#include "../vendor/nlohmann/json.hpp"

#include <cmath>

using json = nlohmann::json;

namespace multisite_player {

namespace {

// What the UI shows in place of a stored secret. Sending it back unchanged
// means "leave it alone", so an operator editing the room name does not have
// to retype the bucket's secret key — and the key never has to leave the box
// to come back again.
constexpr char kSecretPlaceholder[] = "••••••••";

json status_json(const Player& player) {
    Status s;
    player.status(s);

    json j;
    j["room_id"]        = s.room_id;
    j["room_state"]     = s.room_state;
    j["event_id"]       = s.event_id;
    j["pinned_event_id"] = s.pinned_event_id;
    j["live_elsewhere"] = s.live_elsewhere;
    j["live_event_id"]  = s.live_event_id;

    j["playing"]    = s.playing;
    j["paused"]     = s.paused;
    j["buffering"]  = s.buffering;
    j["loading"]    = s.loading;
    j["locked"]     = s.locked;
    j["configured"] = s.configured;

    j["playhead_ms"]    = s.playhead_ms;
    j["live_ms"]        = s.live_ms;
    j["earliest_ms"]    = s.earliest_ms;
    j["started_ms"]     = s.started_ms;
    j["end_ms"]         = s.end_ms;
    j["total_ms"]       = s.total_ms;
    j["seek_target_ms"] = s.seek_target_ms;
    j["behind_live_s"]  = s.behind_live_s;
    j["delay_from_live_s"] = s.delay_from_live_s;
    j["ended"]       = s.ended;
    j["at_end"]      = s.at_end;
    j["was_live"]    = s.was_live;
    j["interrupted"] = s.interrupted;

    j["buffered_ahead_s"]  = s.buffered_ahead_s;
    j["cached_segments"]   = (unsigned long long)s.cached_segments;
    j["downloaded"]        = s.downloaded;
    j["download_failures"] = s.download_failures;
    j["checksum_failures"] = s.checksum_failures;
    j["gaps_waited"]       = s.gaps_waited;
    j["frames_out"]        = s.frames_out;
    j["frames_dropped"]    = s.frames_dropped;
    j["last_error"]        = s.last_error;

    j["video_width"]    = s.video_width;
    j["video_height"]   = s.video_height;
    j["audio_channels"] = s.audio_channels;
    j["channel_labels"] = s.channel_labels;

    j["output_description"] = s.output_description;
    j["audio_description"]  = s.audio_description;
    j["video_output_ok"]    = s.video_output_ok;
    j["audio_output_ok"]    = s.audio_output_ok;

    json spans = json::array();
    for (const auto& sp : s.cached_spans)
        spans.push_back(json{{"from_ms", sp.first}, {"to_ms", sp.second}});
    j["cached_spans"] = std::move(spans);

    json markers = json::array();
    for (const auto& m : s.markers)
        markers.push_back(json{{"label", m.label}, {"id", m.id},
                               {"at_ms", m.at_ms}});
    j["markers"] = std::move(markers);
    j["current_marker"] = s.current_marker;

    j["now_ms"]  = time_info().now_ms;
    j["version"] = player_version();
    return j;
}

json config_json(const Config& c) {
    json j;
    j["endpoint_host"]  = c.endpoint_host;
    j["r2_account_id"]  = c.r2_account_id;
    j["bucket"]         = c.bucket;
    j["access_key_id"]  = c.access_key_id;
    // Never sent out. The UI shows placeholder dots and sends them back
    // untouched unless somebody types a new key.
    j["secret_access_key"] = c.secret_access_key.empty()
                                 ? std::string()
                                 : std::string(kSecretPlaceholder);
    j["secret_set"]     = !c.secret_access_key.empty();
    j["region"]         = c.region;

    j["room_id"]            = c.room_id;
    j["pinned_event_id"]    = c.pinned_event_id;
    j["prebuffer_segments"] = c.prebuffer_segments;
    j["poll_interval_ms"]   = c.poll_interval_ms;
    j["keep_behind_segments"] = c.keep_behind_segments;
    j["buffer_minutes"]     = c.buffer_minutes;
    j["max_cached_segments"] = c.max_cached_segments;
    j["stale_after_ms"]     = c.stale_after_ms;
    j["cache_dir"]          = c.cache_dir;

    j["drm_card"]        = c.drm_card;
    j["connector"]       = c.connector;
    j["out_width"]       = c.out_width;
    j["out_height"]      = c.out_height;
    j["out_fps"]         = c.out_fps;
    j["idle_mode"]       = to_string(c.idle_mode);
    j["idle_image_path"] = c.idle_image_path;

    j["audio_enabled"]  = c.audio_enabled;
    j["alsa_device"]    = c.alsa_device;
    j["audio_channels"] = c.audio_channels;

    j["web_port"] = c.web_port;
    j["web_bind"] = c.web_bind;

    j["auto_play"]         = c.auto_play;
    j["delay_from_live_s"] = c.delay_from_live_s;
    j["locked"]            = c.locked;
    return j;
}

template <typename T>
void take(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return;
    try { out = it->get<T>(); } catch (...) {}
}

// Merge an edit into the config in force. Anything the browser did not send
// keeps its current value, so a partial form cannot wipe settings it never
// showed.
Config apply_edit(Config c, const json& j) {
    take(j, "endpoint_host", c.endpoint_host);
    take(j, "r2_account_id", c.r2_account_id);
    take(j, "bucket",        c.bucket);
    take(j, "access_key_id", c.access_key_id);
    {
        std::string secret;
        take(j, "secret_access_key", secret);
        // The placeholder means "unchanged". An empty string means the
        // operator cleared it deliberately.
        if (secret != kSecretPlaceholder) {
            auto it = j.find("secret_access_key");
            if (it != j.end() && !it->is_null()) c.secret_access_key = secret;
        }
    }
    take(j, "region",  c.region);
    take(j, "room_id", c.room_id);

    take(j, "prebuffer_segments",   c.prebuffer_segments);
    take(j, "poll_interval_ms",     c.poll_interval_ms);
    take(j, "keep_behind_segments", c.keep_behind_segments);
    take(j, "buffer_minutes",       c.buffer_minutes);
    take(j, "max_cached_segments",  c.max_cached_segments);
    take(j, "stale_after_ms",       c.stale_after_ms);
    take(j, "cache_dir",            c.cache_dir);

    take(j, "drm_card",   c.drm_card);
    take(j, "connector",  c.connector);
    take(j, "out_width",  c.out_width);
    take(j, "out_height", c.out_height);
    take(j, "out_fps",    c.out_fps);
    {
        std::string idle;
        take(j, "idle_mode", idle);
        if (!idle.empty()) c.idle_mode = idle_mode_from_string(idle, c.idle_mode);
    }
    take(j, "idle_image_path", c.idle_image_path);

    take(j, "audio_enabled",  c.audio_enabled);
    take(j, "alsa_device",    c.alsa_device);
    take(j, "audio_channels", c.audio_channels);

    take(j, "web_port", c.web_port);
    take(j, "web_bind", c.web_bind);

    take(j, "auto_play",         c.auto_play);
    take(j, "delay_from_live_s", c.delay_from_live_s);
    take(j, "locked",            c.locked);

    // Guard rails, so a mistyped figure cannot make the box unusable from the
    // very interface being used to fix it.
    if (c.poll_interval_ms   < 500)  c.poll_interval_ms = 500;
    if (c.prebuffer_segments < 0)    c.prebuffer_segments = 0;
    if (c.buffer_minutes     < 1)    c.buffer_minutes = 1;
    if (c.max_cached_segments < 10)  c.max_cached_segments = 10;
    if (c.stale_after_ms     < 30000) c.stale_after_ms = 30000;
    if (c.web_port < 1 || c.web_port > 65535) c.web_port = 8080;
    return c;
}

double num_param(const HttpRequest& req, const char* name, double fallback) {
    const std::string v = req.param(name);
    if (v.empty()) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

bool bool_param(const HttpRequest& req, const char* name, bool fallback) {
    const std::string v = req.param(name);
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace

void register_api(HttpServer& server, Player& player, std::string config_path) {

    // ── Status ───────────────────────────────────────────────────────────────
    server.route("GET", "/api/status", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        res.json(status_json(player).dump());
    });

    // Every control answers with the new status, so the interface never has to
    // guess what its own click did.
    auto control = [&player, config_path](const char* name,
                                          std::function<void(const HttpRequest&)> act) {
        return [&player, name, act](const HttpRequest& req, HttpResponse& res) {
            if (player.locked()) {
                res.status = 409;
                res.json(json{{"error", "the controls are locked"},
                              {"locked", true}}.dump());
                return;
            }
            (void)name;
            act(req);
            res.json(status_json(player).dump());
        };
    };

    server.route("POST", "/api/play",   control("play",
        [&player](const HttpRequest&) { player.play(); }));
    server.route("POST", "/api/stop",   control("stop",
        [&player](const HttpRequest&) { player.stop_playback(); }));
    server.route("POST", "/api/hold",   control("hold",
        [&player](const HttpRequest&) { player.pause(); }));
    server.route("POST", "/api/continue", control("continue",
        [&player](const HttpRequest&) { player.resume(); }));
    server.route("POST", "/api/toggle", control("toggle",
        [&player](const HttpRequest&) { player.toggle_pause(); }));
    server.route("POST", "/api/catch-up", control("catch up",
        [&player](const HttpRequest&) { player.jump_to_live(); }));
    server.route("POST", "/api/seek", control("go to",
        [&player](const HttpRequest& req) {
            player.seek_to_time((long long)num_param(req, "ms", 0));
        }));
    server.route("POST", "/api/jog", control("jog",
        [&player](const HttpRequest& req) {
            player.jog(num_param(req, "seconds", 0));
        }));
    server.route("POST", "/api/delay", control("stay behind live",
        [&player](const HttpRequest& req) {
            player.set_delay_from_live(num_param(req, "seconds", 0));
        }));
    server.route("POST", "/api/marker", control("go to cue",
        [&player](const HttpRequest& req) {
            player.jump_to_marker(req.param("id"));
        }));
    server.route("POST", "/api/load", control("load",
        [&player](const HttpRequest& req) {
            const std::string id = req.param("event");
            if (id.empty()) player.unpin_event(); else player.pin_event(id);
        }));
    server.route("POST", "/api/follow-live", control("follow live",
        [&player](const HttpRequest&) { player.unpin_event(); }));

    // Deliberately NOT behind the lock: locking is a control, and a lock you
    // cannot undo from the interface is a fault, not a safeguard.
    server.route("POST", "/api/lock", [&player, config_path](
                                          const HttpRequest& req,
                                          HttpResponse& res) {
        player.set_locked(bool_param(req, "on", true));
        Config c = player.config();
        std::string err;
        c.save(config_path, err);
        res.json(status_json(player).dump());
    });

    // ── Event list ───────────────────────────────────────────────────────────
    server.route("GET", "/api/events", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        EventListing listing;
        player.event_listing(listing);

        json rows = json::array();
        for (const auto& e : listing.events)
            rows.push_back(json{{"event_id", e.event_id},
                                {"started_ms", e.started_ms},
                                {"duration_s", e.duration_s},
                                {"state", e.state}});
        res.json(json{{"events", std::move(rows)},
                      {"loading", listing.loading},
                      {"listed_once", listing.listed_once},
                      {"fallback_scan", listing.fallback_scan},
                      {"skipped", listing.skipped},
                      {"error", listing.error}}.dump());
    });

    server.route("POST", "/api/events/refresh", [&player](const HttpRequest&,
                                                          HttpResponse& res) {
        player.refresh_events();
        res.json(json{{"ok", true}}.dump());
    });

    // ── Settings ─────────────────────────────────────────────────────────────
    server.route("GET", "/api/config", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        res.json(config_json(player.config()).dump());
    });

    server.route("PUT", "/api/config", [&player, config_path](
                                           const HttpRequest& req,
                                           HttpResponse& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& e) {
            res.status = 400;
            res.json(json{{"error", std::string("bad request: ") + e.what()}}.dump());
            return;
        }
        Config updated = apply_edit(player.config(), body);

        std::string err;
        if (!updated.save(config_path, err)) {
            // Saving is what makes a setting survive the next power cut, so a
            // failure here must be reported rather than silently applied.
            res.status = 500;
            res.json(json{{"error", "could not save settings: " + err}}.dump());
            return;
        }
        player.reconfigure(updated);
        res.json(config_json(player.config()).dump());
    });

    // What this box can actually be set to. Listed from the hardware rather
    // than typed into a form, so a setting the display would refuse cannot be
    // chosen in the first place.
    server.route("GET", "/api/outputs", [&player](const HttpRequest&,
                                                  HttpResponse& res) {
        json displays = json::array();
        for (const auto& d : player.video().displays()) {
            json modes = json::array();
            for (const auto& m : d.modes)
                modes.push_back(json{{"width", m.width}, {"height", m.height},
                                     {"refresh_mhz", m.refresh_mhz},
                                     {"preferred", m.preferred}});
            displays.push_back(json{{"connector", d.connector},
                                    {"connected", d.connected},
                                    {"monitor_name", d.monitor_name},
                                    {"modes", std::move(modes)}});
        }
        json devices = json::array();
        for (const auto& a : player.audio().devices())
            devices.push_back(json{{"id", a.id},
                                   {"description", a.description},
                                   {"max_channels", a.max_channels}});
        res.json(json{{"displays", std::move(displays)},
                      {"audio_devices", std::move(devices)},
                      {"display_in_use", player.video().description()},
                      {"audio_in_use", player.audio().description()}}.dump());
    });

    // ── The box ──────────────────────────────────────────────────────────────
    server.route("GET", "/api/system", [&player](const HttpRequest&,
                                                 HttpResponse& res) {
        const SystemInfo sys = system_info();
        const TimeInfo   t   = time_info();
        const Config     cfg = player.config();
        const DiskInfo   disk = disk_info(cfg.cache_dir);

        json nets = json::array();
        for (const auto& n : network_interfaces())
            nets.push_back(json{{"name", n.name}, {"ipv4", n.ipv4},
                                {"mac", n.mac}, {"up", n.up},
                                {"wireless", n.wireless}});

        res.json(json{
            {"hostname", hostname()},
            {"version", player_version()},
            {"model", sys.model},
            {"os_version", sys.os_version},
            {"kernel", sys.kernel},
            {"uptime_s", sys.uptime_s},
            {"load_1min", sys.load_1min},
            {"cpu_temp_c", sys.cpu_temp_c},
            {"throttled", sys.throttled},
            {"under_voltage", sys.under_voltage},
            {"interfaces", std::move(nets)},
            {"time", json{{"now_ms", t.now_ms},
                          {"local_time", t.local_time},
                          {"timezone", t.timezone},
                          {"ntp_enabled", t.ntp_enabled},
                          {"ntp_synchronised", t.ntp_synchronised}}},
            {"disk", json{{"path", disk.path},
                          {"total_bytes", disk.total_bytes},
                          {"free_bytes", disk.free_bytes},
                          {"is_sd_card", disk.is_sd_card}}},
        }.dump());
    });

    server.route("GET", "/api/system/timezones", [](const HttpRequest&,
                                                    HttpResponse& res) {
        res.json(json{{"timezones", available_timezones()}}.dump());
    });

    server.route("POST", "/api/system/time", [](const HttpRequest& req,
                                                HttpResponse& res) {
        std::string err;
        if (!req.param("timezone").empty())
            err = set_timezone(req.param("timezone"));
        if (err.empty() && !req.param("ntp").empty())
            err = set_ntp(bool_param(req, "ntp", true));
        if (err.empty() && !req.param("epoch_ms").empty())
            err = set_time((long long)num_param(req, "epoch_ms", 0));

        if (!err.empty()) {
            res.status = 500;
            res.json(json{{"error", err}}.dump());
            return;
        }
        const TimeInfo t = time_info();
        res.json(json{{"now_ms", t.now_ms}, {"local_time", t.local_time},
                      {"timezone", t.timezone},
                      {"ntp_enabled", t.ntp_enabled},
                      {"ntp_synchronised", t.ntp_synchronised}}.dump());
    });

    server.route("POST", "/api/system/restart", [](const HttpRequest&,
                                                   HttpResponse& res) {
        restart_service();
        res.json(json{{"ok", true}, {"message", "restarting"}}.dump());
    });
    server.route("POST", "/api/system/reboot", [](const HttpRequest&,
                                                  HttpResponse& res) {
        reboot_box();
        res.json(json{{"ok", true}, {"message", "rebooting"}}.dump());
    });
    server.route("POST", "/api/system/shutdown", [](const HttpRequest&,
                                                    HttpResponse& res) {
        shutdown_box();
        res.json(json{{"ok", true}, {"message", "shutting down"}}.dump());
    });

    // ── Preview ──────────────────────────────────────────────────────────────
    // One JPEG per request rather than a stream: a dropped connection costs a
    // single frame instead of the whole preview, and a phone that goes to
    // sleep stops asking without leaving anything encoding on the box.
    //
    // The encoder is shared and serialised: several browsers looking at once
    // must not each start their own.
    {
        auto encoder = std::make_shared<JpegEncoder>();
        auto encoder_mtx = std::make_shared<std::mutex>();
        server.route("GET", "/preview.jpg", [&player, encoder, encoder_mtx](
                                                const HttpRequest& req,
                                                HttpResponse& res) {
            multisite::DecodedVideoFrame frame;
            uint64_t version = 0;
            if (!player.latest_frame(frame, version)) {
                res.status = 503;
                res.content_type = "text/plain; charset=utf-8";
                res.body = "nothing decoded yet";
                return;
            }
            int width = (int)num_param(req, "width", 640);
            width = std::max(160, std::min(1920, width));
            int quality = (int)num_param(req, "quality", 70);
            quality = std::max(10, std::min(95, quality));

            std::vector<uint8_t> jpeg;
            std::string err;
            bool ok;
            {
                std::lock_guard<std::mutex> lk(*encoder_mtx);
                ok = encoder->encode(frame, width, quality, jpeg, err);
            }
            if (!ok) {
                res.status = 503;
                res.content_type = "text/plain; charset=utf-8";
                res.body = err;
                return;
            }
            res.content_type = "image/jpeg";
            res.headers["X-Frame-Version"] = std::to_string(version);
            res.body.assign(jpeg.begin(), jpeg.end());
        });
    }

    // ── Log ──────────────────────────────────────────────────────────────────
    // An operator with a phone and no SSH should still be able to see why
    // nothing is playing.
    server.route("GET", "/api/log", [](const HttpRequest& req,
                                       HttpResponse& res) {
        const size_t lines = (size_t)num_param(req, "lines", 120);
        json rows = json::array();
        for (const auto& e : recent_log(lines))
            rows.push_back(json{{"at_ms", e.at_ms},
                                {"level", to_string(e.level)},
                                {"text", e.text}});
        res.json(json{{"lines", std::move(rows)}}.dump());
    });
}

} // namespace multisite_player
