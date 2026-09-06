#include "api.h"
#include "log.h"
#include "stream_plan.h"

#include "nlohmann/json.hpp"

using json = nlohmann::json;
using multisite_player::HttpRequest;
using multisite_player::HttpResponse;
using multisite_player::HttpServer;

namespace multisite_relay {

namespace {

json body_json(const HttpRequest& req) {
    if (req.body.empty()) return json::object();
    try {
        auto j = json::parse(req.body);
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

std::string str(const json& j, const char* key, const std::string& fb = "") {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return fb;
    return it->get<std::string>();
}

int num(const json& j, const char* key, int fb = 0) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fb;
    return it->get<int>();
}

bool flag(const json& j, const char* key, bool fb = false) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return fb;
    return it->get<bool>();
}

void fail(HttpResponse& res, int code, const std::string& message) {
    json j; j["error"] = message;
    res.status = code;
    res.json(j.dump());
}

void ok(HttpResponse& res) {
    json j; j["ok"] = true;
    res.json(j.dump());
}

int64_t id_of(const HttpRequest& req) {
    const std::string s = req.param("id");
    try { return s.empty() ? 0 : std::stoll(s); } catch (...) { return 0; }
}

// A destination as the browser is allowed to see it. The stream key never
// leaves the container: the UI shows whether one is set, not what it is.
json dest_json(const Destination& d) {
    json j;
    j["id"] = d.id;
    j["name"] = d.name;
    j["room_id"] = d.room_id;
    j["url"] = d.url;
    j["has_key"] = !d.stream_key.empty();
    j["audio_label"] = d.audio.label;
    j["allow_transcode"] = d.allow_transcode;
    j["enabled"] = d.enabled;
    j["delay_s"] = d.delay_s;
    return j;
}

json status_json(const RelayStatus& s) {
    json j;
    j["id"] = s.id;
    j["name"] = s.name;
    j["state"] = s.state;
    j["state_text"] = s.state_text;
    j["detail"] = s.detail;
    j["error"] = s.error;
    j["enabled"] = s.enabled;
    j["live"] = s.live;
    j["uptime_s"] = s.uptime_s;
    j["behind_live_s"] = s.behind_live_s;
    j["bitrate_kbps"] = s.bitrate_kbps;
    j["restarts"] = s.restarts;
    j["sent_bytes"] = s.sent_bytes;
    j["audio_label"] = s.audio_label;
    return j;
}

} // namespace

void register_routes(HttpServer& server, Service& service) {

    server.route("GET", "/api/status", [&service](const HttpRequest&,
                                                  HttpResponse& res) {
        const auto s = service.status();
        json j;
        j["storage_configured"] = s.storage_configured;
        j["room_id"] = s.room_id;
        j["room_state"] = s.room_state;
        j["room_state_text"] = s.room_state_text;
        j["event_id"] = s.event_id;
        j["storage_error"] = s.storage_error;
        j["video"] = s.video_summary;
        j["can_send"] = s.can_send;
        j["cannot_send_reason"] = s.cannot_send_reason;
        j["total_out_kbps"] = s.total_out_kbps;
        j["audio_labels"] = s.audio_labels;
        json d = json::array();
        for (const auto& x : s.destinations) d.push_back(status_json(x));
        j["destinations"] = d;
        res.json(j.dump());
    });

    // ── Storage and room ─────────────────────────────────────────────────────

    server.route("GET", "/api/config", [&service](const HttpRequest&,
                                                  HttpResponse& res) {
        const auto c = service.config().storage();
        const auto r = service.config().room();
        json j;
        j["endpoint_host"] = c.endpoint_host;
        j["r2_account_id"] = c.r2_account_id;
        j["bucket"] = c.bucket;
        j["access_key_id"] = c.access_key_id;
        // The secret is never sent back, only whether there is one.
        j["has_secret"] = !c.secret_access_key.empty();
        j["region"] = c.region;
        j["use_https"] = c.use_https;
        j["room_id"] = r.room_id;
        j["default_delay_s"] = r.default_delay_s;
        res.json(j.dump());
    });

    server.route("PUT", "/api/config", [&service](const HttpRequest& req,
                                                  HttpResponse& res) {
        const auto j = body_json(req);
        auto c = service.config().storage();
        c.endpoint_host = str(j, "endpoint_host", c.endpoint_host);
        c.r2_account_id = str(j, "r2_account_id", c.r2_account_id);
        c.bucket = str(j, "bucket", c.bucket);
        c.access_key_id = str(j, "access_key_id", c.access_key_id);
        // An empty secret means "leave it alone", so saving the form without
        // retyping the key does not wipe it.
        const std::string secret = str(j, "secret_access_key");
        if (!secret.empty()) c.secret_access_key = secret;
        c.region = str(j, "region", c.region);
        if (c.region.empty()) c.region = "auto";
        c.use_https = flag(j, "use_https", c.use_https);

        auto r = service.config().room();
        r.room_id = str(j, "room_id", r.room_id);
        r.default_delay_s = num(j, "default_delay_s", r.default_delay_s);
        if (r.default_delay_s < 0 || r.default_delay_s > 3600)
            return fail(res, 400, "The delay should be between 0 seconds and "
                                  "an hour.");
        if (r.room_id.empty())
            return fail(res, 400, "Enter the feed name used at the main site.");

        service.config().set_storage(c);
        service.config().set_room(r);
        service.reload();
        ok(res);
    });

    server.route("POST", "/api/storage/test", [&service](const HttpRequest&,
                                                         HttpResponse& res) {
        const std::string e = service.check_storage();
        json j;
        j["ok"] = e.empty();
        j["error"] = e;
        res.json(j.dump());
    });

    // ── Destinations ─────────────────────────────────────────────────────────

    server.route("GET", "/api/destinations", [&service](const HttpRequest&,
                                                        HttpResponse& res) {
        json out = json::array();
        for (const auto& d : service.config().destinations())
            out.push_back(dest_json(d));
        res.json(out.dump());
    });

    server.route("POST", "/api/destinations", [&service](const HttpRequest& req,
                                                         HttpResponse& res) {
        const auto j = body_json(req);
        Destination d;
        d.name = str(j, "name");
        d.room_id = service.config().room().room_id;
        d.url = str(j, "url");
        d.stream_key = str(j, "stream_key");
        d.audio.label = str(j, "audio_label");
        d.allow_transcode = flag(j, "allow_transcode");
        d.delay_s = num(j, "delay_s", 0);
        d.enabled = false;      // added stopped; the operator presses Start

        std::string error;
        const int64_t id = service.config().add(d, error);
        if (!id) return fail(res, 400, error);
        service.reload();
        json r; r["id"] = id;
        res.json(r.dump());
    });

    server.route("POST", "/api/destinations/update",
                 [&service](const HttpRequest& req, HttpResponse& res) {
        const auto j = body_json(req);
        const int64_t id = id_of(req);
        auto existing = service.config().destination(id);
        if (!existing) return fail(res, 404, "That destination no longer exists.");

        Destination d = *existing;
        d.name = str(j, "name", d.name);
        d.url = str(j, "url", d.url);
        const std::string key = str(j, "stream_key");
        if (!key.empty()) d.stream_key = key;   // blank means "unchanged"
        d.audio.label = str(j, "audio_label", d.audio.label);
        d.allow_transcode = flag(j, "allow_transcode", d.allow_transcode);
        d.delay_s = num(j, "delay_s", d.delay_s);

        std::string error;
        if (!service.config().update(d, error)) return fail(res, 400, error);
        service.reload();
        ok(res);
    });

    server.route("POST", "/api/destinations/delete",
                 [&service](const HttpRequest& req, HttpResponse& res) {
        const int64_t id = id_of(req);
        if (!service.config().remove(id))
            return fail(res, 404, "That destination no longer exists.");
        service.reload();
        ok(res);
    });

    server.route("POST", "/api/destinations/start",
                 [&service](const HttpRequest& req, HttpResponse& res) {
        const int64_t id = id_of(req);
        auto d = service.config().destination(id);
        if (!d) return fail(res, 404, "That destination no longer exists.");
        // Refuse here as well as in the relay, so pressing Start on something
        // that cannot work says why immediately instead of failing quietly a
        // second later.
        const auto s = service.status();
        if (s.storage_configured && !s.can_send && !s.cannot_send_reason.empty())
            return fail(res, 409, s.cannot_send_reason);
        service.set_enabled(id, true);
        rlog_info("[%s] started by the operator", d->name.c_str());
        ok(res);
    });

    server.route("POST", "/api/destinations/stop",
                 [&service](const HttpRequest& req, HttpResponse& res) {
        const int64_t id = id_of(req);
        auto d = service.config().destination(id);
        if (!d) return fail(res, 404, "That destination no longer exists.");
        service.set_enabled(id, false);
        rlog_info("[%s] stopped by the operator", d->name.c_str());
        ok(res);
    });

    // ── Log ──────────────────────────────────────────────────────────────────

    server.route("GET", "/api/log", [](const HttpRequest& req,
                                       HttpResponse& res) {
        size_t n = 200;
        try {
            const std::string l = req.param("lines");
            if (!l.empty()) n = (size_t)std::stoul(l);
        } catch (...) {}
        json out = json::array();
        for (const auto& e : recent_log(n)) {
            json j;
            j["at_ms"] = e.at_ms;
            j["level"] = to_string(e.level);
            j["text"] = e.text;
            out.push_back(j);
        }
        res.json(out.dump());
    });
}

} // namespace multisite_relay
