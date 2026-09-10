// SPDX-License-Identifier: GPL-3.0-or-later
#include "encoder_api.h"
#include "api_common.h"
#include "commands.h"
#include "ui_thread.h"
#include "web_ui.h"

#include "core/control_api.h"

#include <obs-module.h>

#include <functional>
#include <memory>
#include <string>

namespace multisite_obs {

namespace {

using ControlAction =
    std::function<void(const multisite::HttpRequest&, std::string&)>;

// A control: refused while locked, run on OBS's UI thread, and answered with the
// new status so the page never has to guess what its own click did.
//
// The bodies themselves live in commands.cpp, because obs-websocket drives the
// same ones — see §8.3, "no new control logic, and no second path to keep in
// step". This wrapper is only the HTTP-shaped part: the lock, the thread, and
// the reply.
multisite::HttpHandler encoder_control(ControlAction act) {
    return [act](const multisite::HttpRequest& req,
                 multisite::HttpResponse& res) {
        // Checked before anything else: while OBS is closing, a control
        // accepted now would be applied to something already going away.
        if (web_ui_stopping()) {
            fail(res, 503, "OBS is closing");
            return;
        }
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
        res.json(encoder_status_json());
    };
}

// The one place a route path is written: the name list in the core, prefixed
// with /api/. What a page calls and what a vendor request is named therefore
// cannot drift.
std::string path(const char* name) { return multisite::api_path(name); }

} // namespace

void register_encoder_api(multisite::HttpServer& server) {
    using multisite::HttpRequest;
    using multisite::HttpResponse;

    server.route("GET", path("encoder/status"),
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(encoder_status_json());
    });

    server.route("POST", path("encoder/go-live"), encoder_control(
        [](const HttpRequest& req, std::string& error) {
            // The title arrives in the body or the query, whichever the page
            // found easier. Blank means "name it now", which is what the dock
            // does with a field nobody touched.
            const json j = body_object(req);
            std::string name;
            if (!json_str(j, "event_name", name)) name = str_param(req, "name");
            encoder_go_live(name, error);
        }));

    server.route("POST", path("encoder/end"), encoder_control(
        [](const HttpRequest&, std::string& error) {
            encoder_end(error);
        }));

    server.route("POST", path("encoder/marker"), encoder_control(
        [](const HttpRequest& req, std::string& error) {
            encoder_marker(str_param(req, "label"), error);
        }));

    server.route("GET", path("encoder/settings"),
                 [](const HttpRequest&, HttpResponse& res) {
        res.json(encoder_settings_json());
    });

    server.route("POST", path("encoder/settings"),
                 [](const HttpRequest& req, HttpResponse& res) {
        if (web_ui_locked()) { fail(res, 409, "the controls are locked"); return; }

        const std::string body = req.body;
        if (!run_on_ui_thread([body] {
                std::string error;
                encoder_apply_settings(body, error);
            }, 10000)) {
            fail(res, 503, "OBS is busy - try again in a moment");
            return;
        }
        res.json(encoder_settings_json());
    });
}

} // namespace multisite_obs
