#include "config.h"

#include "../vendor/nlohmann/json.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

namespace multisite_player {

const char* to_string(IdleMode m) {
    switch (m) {
    case IdleMode::Black:     return "black";
    case IdleMode::HoldFrame: return "hold";
    case IdleMode::Splash:    return "splash";
    case IdleMode::Image:     return "image";
    }
    return "splash";
}

IdleMode idle_mode_from_string(const std::string& s, IdleMode fallback) {
    if (s == "black")  return IdleMode::Black;
    if (s == "hold")   return IdleMode::HoldFrame;
    if (s == "splash") return IdleMode::Splash;
    if (s == "image")  return IdleMode::Image;
    return fallback;
}

const char* default_config_path() {
    return "/etc/multisite-player/config.json";
}

namespace {

// Read a value only when the file actually carries it, so a config written by
// an older version keeps this version's defaults for anything it never knew
// about — rather than resetting those fields to zero.
template <typename T>
void take(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        try { out = it->get<T>(); } catch (...) { /* keep the default */ }
    }
}

} // namespace

bool Config::load(const std::string& path, std::string& error) {
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        // Not an error. A box with no config still boots and shows its splash.
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    json j;
    try {
        j = json::parse(ss.str());
    } catch (const std::exception& e) {
        error = std::string("config is not valid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        error = "config is not a JSON object";
        return false;
    }

    take(j, "endpoint_host",        endpoint_host);
    take(j, "r2_account_id",        r2_account_id);
    take(j, "bucket",               bucket);
    take(j, "access_key_id",        access_key_id);
    take(j, "secret_access_key",    secret_access_key);
    take(j, "region",               region);

    take(j, "room_id",              room_id);
    take(j, "pinned_event_id",      pinned_event_id);

    take(j, "prebuffer_segments",   prebuffer_segments);
    take(j, "poll_interval_ms",     poll_interval_ms);
    take(j, "keep_behind_segments", keep_behind_segments);
    take(j, "buffer_minutes",       buffer_minutes);
    take(j, "max_cached_segments",  max_cached_segments);
    take(j, "stale_after_ms",       stale_after_ms);
    take(j, "cache_dir",            cache_dir);

    take(j, "drm_card",             drm_card);
    take(j, "connector",            connector);
    take(j, "out_width",            out_width);
    take(j, "out_height",           out_height);
    take(j, "out_fps",              out_fps);

    std::string idle = to_string(idle_mode);
    take(j, "idle_mode", idle);
    idle_mode = idle_mode_from_string(idle, idle_mode);
    take(j, "idle_image_path",      idle_image_path);

    take(j, "audio_enabled",        audio_enabled);
    take(j, "alsa_device",          alsa_device);
    take(j, "audio_channels",       audio_channels);

    take(j, "web_port",             web_port);
    take(j, "web_bind",             web_bind);

    take(j, "auto_play",            auto_play);
    take(j, "delay_from_live_s",    delay_from_live_s);
    take(j, "locked",               locked);
    return true;
}

bool Config::save(const std::string& path, std::string& error) const {
    error.clear();

    json j;
    j["endpoint_host"]        = endpoint_host;
    j["r2_account_id"]        = r2_account_id;
    j["bucket"]               = bucket;
    j["access_key_id"]        = access_key_id;
    j["secret_access_key"]    = secret_access_key;
    j["region"]               = region;

    j["room_id"]              = room_id;
    j["pinned_event_id"]      = pinned_event_id;

    j["prebuffer_segments"]   = prebuffer_segments;
    j["poll_interval_ms"]     = poll_interval_ms;
    j["keep_behind_segments"] = keep_behind_segments;
    j["buffer_minutes"]       = buffer_minutes;
    j["max_cached_segments"]  = max_cached_segments;
    j["stale_after_ms"]       = stale_after_ms;
    j["cache_dir"]            = cache_dir;

    j["drm_card"]             = drm_card;
    j["connector"]            = connector;
    j["out_width"]            = out_width;
    j["out_height"]           = out_height;
    j["out_fps"]              = out_fps;
    j["idle_mode"]            = to_string(idle_mode);
    j["idle_image_path"]      = idle_image_path;

    j["audio_enabled"]        = audio_enabled;
    j["alsa_device"]          = alsa_device;
    j["audio_channels"]       = audio_channels;

    j["web_port"]             = web_port;
    j["web_bind"]             = web_bind;

    j["auto_play"]            = auto_play;
    j["delay_from_live_s"]    = delay_from_live_s;
    j["locked"]               = locked;

    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
        out << j.dump(2) << "\n";
        out.flush();
        if (!out) {
            error = "failed writing " + tmp;
            return false;
        }
    }
    // The secret key lives in here. Lock it down before it is in place under
    // its real name, so it is never briefly world-readable.
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);

    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot replace " + path;
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace multisite_player
