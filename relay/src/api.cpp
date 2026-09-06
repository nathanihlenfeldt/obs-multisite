#include "api.h"
#include "auth.h"
#include "room_feeder.h"
#include "log.h"
#include "stream_plan.h"

#include "nlohmann/json.hpp"

#include <cstdint>
#include <ctime>
#include <string>

using json = nlohmann::json;
using multisite_player::HttpHandler;
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

void register_routes(HttpServer& server, Service& service, Auth& auth) {

    // ── The guard ────────────────────────────────────────────────────────────
    // The failure mode of getting this wrong is not "a stranger reads a status
    // page" — it is a stranger changing where a church's service is sent. So
    // every route registered through `route()` below is behind a session, and
    // the three that cannot be (signing in, and asking whether you are signed
    // in) are registered directly on the server and are listed here:
    //
    //     GET    /api/session   is anyone signed in, and is this private
    //     POST   /api/session   sign in, or claim an unclaimed relay
    //     DELETE /api/session   sign out
    //
    // Routes added later use route() and are protected by default. That is
    // deliberate: protection by forgetting, rather than by remembering.
    auto guarded = [&auth](HttpHandler h) -> HttpHandler {
        return [&auth, h](const HttpRequest& req, HttpResponse& res) {
            if (!auth.configured()) {
                json j;
                j["error"] = "This relay has no login yet.";
                j["needs_setup"] = true;
                res.status = 409;
                res.json(j.dump());
                return;
            }
            auto c = req.headers.find("cookie");
            const std::string token =
                c == req.headers.end() ? std::string()
                                       : Auth::session_from_cookies(c->second);
            if (!auth.valid_session(token)) {
                json j;
                j["error"] = "Please sign in.";
                res.status = 401;
                res.json(j.dump());
                return;
            }
            h(req, res);
        };
    };
    auto route = [&server, &guarded](const std::string& verb,
                                     const std::string& path, HttpHandler h) {
        server.route(verb, path, guarded(std::move(h)));
    };

    // ── Signing in ───────────────────────────────────────────────────────────
    server.route("GET", "/api/session", [&auth](const HttpRequest& req,
                                                HttpResponse& res) {
        auto c = req.headers.find("cookie");
        const std::string token =
            c == req.headers.end() ? std::string()
                                   : Auth::session_from_cookies(c->second);
        json j;
        j["configured"] = auth.configured();
        j["signed_in"] = auth.valid_session(token);
        // Reported so the interface can say so, not so it can refuse. Locking
        // an operator out of their own relay mid-service would be the worse
        // failure by far.
        j["connection_is_private"] = connection_is_private(req.headers);
        res.json(j.dump());
    });

    server.route("POST", "/api/session", [&auth](const HttpRequest& req,
                                                 HttpResponse& res) {
        const auto j = body_json(req);
        const std::string user = str(j, "username");
        const std::string pass = str(j, "password");

        // First run: whoever arrives first sets the login. The container and
        // the port belong to the church, so this is narrower than it sounds —
        // but it is exactly why the documentation says to set the login before
        // opening the port to anyone else.
        if (!auth.configured()) {
            const std::string e = auth.set_credentials(user, pass);
            if (!e.empty()) return fail(res, 400, e);
            rlog_info("operator login created for \"%s\"", user.c_str());
        } else if (!auth.verify(user, pass)) {
            // Deliberately does not say which of the two was wrong.
            rlog_warn("failed sign-in for \"%s\"", user.c_str());
            return fail(res, 401, "That username or password is not right.");
        }

        const std::string token = auth.create_session();
        if (token.empty()) return fail(res, 500, "Could not start a session.");
        res.headers["Set-Cookie"] =
            Auth::cookie_for(token, connection_is_private(req.headers));
        ok(res);
    });

    server.route("DELETE", "/api/session", [&auth](const HttpRequest& req,
                                                   HttpResponse& res) {
        auto c = req.headers.find("cookie");
        if (c != req.headers.end())
            auth.destroy_session(Auth::session_from_cookies(c->second));
        res.headers["Set-Cookie"] = Auth::clear_cookie();
        ok(res);
    });

    // Changing the password needs the current one, so a session left open on
    // an unattended machine cannot be used to lock the operator out.
    route("POST", "/api/password", [&auth](const HttpRequest& req,
                                           HttpResponse& res) {
        const auto j = body_json(req);
        const std::string user = str(j, "username");
        if (!auth.verify(user, str(j, "current_password")))
            return fail(res, 401, "That current password is not right.");
        const std::string e = auth.set_credentials(user, str(j, "new_password"));
        if (!e.empty()) return fail(res, 400, e);
        rlog_info("operator password changed");
        ok(res);
    });

    route("GET", "/api/status", [&service](const HttpRequest&,
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

    route("GET", "/api/config", [&service](const HttpRequest&,
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

    route("PUT", "/api/config", [&service](const HttpRequest& req,
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

    route("POST", "/api/storage/test", [&service](const HttpRequest&,
                                                         HttpResponse& res) {
        const std::string e = service.check_storage();
        json j;
        j["ok"] = e.empty();
        j["error"] = e;
        res.json(j.dump());
    });

    // ── Destinations ─────────────────────────────────────────────────────────

    route("GET", "/api/destinations", [&service](const HttpRequest&,
                                                        HttpResponse& res) {
        json out = json::array();
        for (const auto& d : service.config().destinations())
            out.push_back(dest_json(d));
        res.json(out.dump());
    });

    route("POST", "/api/destinations", [&service](const HttpRequest& req,
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

    route("POST", "/api/destinations/update",
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

    route("POST", "/api/destinations/delete",
                 [&service](const HttpRequest& req, HttpResponse& res) {
        const int64_t id = id_of(req);
        if (!service.config().remove(id))
            return fail(res, 404, "That destination no longer exists.");
        service.reload();
        ok(res);
    });

    route("POST", "/api/destinations/start",
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

    route("POST", "/api/destinations/stop",
                 [&service](const HttpRequest& req, HttpResponse& res) {
        const int64_t id = id_of(req);
        auto d = service.config().destination(id);
        if (!d) return fail(res, 404, "That destination no longer exists.");
        service.set_enabled(id, false);
        rlog_info("[%s] stopped by the operator", d->name.c_str());
        ok(res);
    });

    // ── Past services ────────────────────────────────────────────────────────

    route("GET", "/api/events", [&service](const HttpRequest& req,
                                           HttpResponse& res) {
        const bool force = req.param("refresh") == "1";
        json out = json::array();
        for (const auto& e : service.events(force)) {
            json j;
            j["event_id"] = e.event_id;
            j["started_at_ms"] = e.started_at_ms;
            j["duration_s"] = e.duration_s;
            j["state"] = multisite::to_string(e.state);
            // The one thing the interface needs to decide what to offer: a
            // service still going out can be neither downloaded nor
            // rebroadcast, because it has no end yet.
            j["finished"] = e.state != multisite::EventState::Live;
            j["interrupted"] = e.state == multisite::EventState::Interrupted;
            out.push_back(j);
        }
        res.json(out.dump());
    });

    route("GET", "/api/events/download", [&service](const HttpRequest& req,
                                                    HttpResponse& res) {
        const std::string id = req.param("event");
        const std::string problem = service.check_event_is_finished(id);
        if (!problem.empty()) return fail(res, 409, problem);

        RoomFeeder* feeder = service.feeder();
        if (!feeder) return fail(res, 409, "Storage has not been set up yet.");

        // Named for when the service happened, because a folder of ULIDs is
        // no use to anyone looking for last Sunday.
        std::string name = "service";
        for (const auto& e : service.events()) {
            if (e.event_id != id) continue;
            const std::time_t t = (std::time_t)(e.started_at_ms / 1000);
            char buf[32];
            if (std::strftime(buf, sizeof(buf), "%Y-%m-%d-%H%M",
                              std::localtime(&t)))
                name = std::string("service-") + buf;
            break;
        }

        // Listed once, then both measured and sent from that same list, so
        // the promised length and the bytes that follow cannot disagree.
        std::string list_error;
        auto parts = feeder->event_parts(id, list_error);
        if (parts.empty()) return fail(res, 502, list_error);

        int64_t size = 0;
        for (const auto& p : parts) {
            if (p.size < 0) { size = -1; break; }
            size += p.size;
        }

        res.content_type = "video/mp4";
        res.headers["Content-Disposition"] =
            "attachment; filename=\"" + name + ".mp4\"";
        if (size > 0) res.headers["Content-Length"] = std::to_string(size);

        res.stream = [feeder, id, parts](multisite_player::HttpStream& out) {
            std::string error;
            feeder->stream_parts(parts, [&out](const uint8_t* p, size_t n) {
                return out.write(p, n);     // false once the browser goes away
            }, error);
            if (!error.empty())
                rlog_warn("download of %s ended: %s", id.c_str(), error.c_str());
        };
    });

    // ── Rebroadcast ──────────────────────────────────────────────────────────

    route("GET", "/api/rebroadcast", [&service](const HttpRequest&,
                                                HttpResponse& res) {
        json j;
        j["running"] = service.rebroadcasting();
        j["event_id"] = service.rebroadcast_event();
        if (service.rebroadcasting())
            j["status"] = status_json(service.rebroadcast_status());
        res.json(j.dump());
    });

    route("POST", "/api/rebroadcast", [&service](const HttpRequest& req,
                                                 HttpResponse& res) {
        const auto j = body_json(req);
        const std::string e =
            service.start_rebroadcast(str(j, "event_id"),
                                      (int64_t)num(j, "destination_id", 0));
        if (!e.empty()) return fail(res, 409, e);
        ok(res);
    });

    route("DELETE", "/api/rebroadcast", [&service](const HttpRequest&,
                                                   HttpResponse& res) {
        service.stop_rebroadcast();
        ok(res);
    });

    // ── Log ──────────────────────────────────────────────────────────────────

    route("GET", "/api/log", [](const HttpRequest& req,
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
