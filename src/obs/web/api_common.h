// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// api_common.h — the shared vocabulary of the remote-control APIs.
//
// Both halves of the plugin answer requests the same way, because both are read
// by the same sort of person on the same sort of phone: a control returns the
// new status in the same response, a stored secret is never sent to a browser,
// and a refusal says why in words rather than in a status code.
//
#include "core/http_server.h"

#include "vendor/nlohmann/json.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace multisite_obs {

using nlohmann::json;

// What a page is shown in place of a stored secret — and what it sends back to
// mean "leave that alone". Written as bytes rather than as the characters
// themselves, because a narrow literal's encoding on Windows is whatever
// codepage the compiler assumed, and this has to reach a browser looking
// exactly like the campus player's does.
constexpr char kSecretPlaceholder[] =
    "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"
    "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2";

inline long long now_ms() {
    return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ── Query-string parameters ──────────────────────────────────────────────────

inline std::string str_param(const multisite::HttpRequest& req,
                             const char* name) {
    return req.param(name);
}

inline double num_param(const multisite::HttpRequest& req, const char* name,
                        double fallback) {
    const std::string v = req.param(name);
    if (v.empty()) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

inline bool bool_param(const multisite::HttpRequest& req, const char* name,
                       bool fallback) {
    const std::string v = req.param(name);
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// ── JSON body fields ─────────────────────────────────────────────────────────
// Each returns false when the field is absent or the wrong type, so a page that
// posts a partial document changes only what it actually sent.

inline bool json_str(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

inline bool json_num(const json& j, const char* key, double& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

inline bool json_int(const json& j, const char* key, int& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    out = (int)it->get<double>();
    return true;
}

inline bool json_bool(const json& j, const char* key, bool& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

// The body of a POST, as an object. Anything else — including an empty body,
// which is what a button with nothing to say sends — is an empty object rather
// than an error, so `POST /api/decoder/play` needs no JSON at all.
inline json body_object(const multisite::HttpRequest& req) {
    if (req.body.empty()) return json::object();
    try {
        auto j = json::parse(req.body);
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

inline void fail(multisite::HttpResponse& res, int code,
                 const std::string& message) {
    res.status = code;
    res.json(json{{"error", message}}.dump());
}

} // namespace multisite_obs