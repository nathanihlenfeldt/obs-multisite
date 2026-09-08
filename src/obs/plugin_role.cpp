// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_role.h"
#include "plugin_log.h"

#include <obs-module.h>
#include <util/platform.h>

#include <mutex>

namespace multisite_obs {

const char* role_key(Role r) {
    switch (r) {
        case Role::EncoderOnly: return "encoder";
        case Role::DecoderOnly: return "decoder";
        default:                return "both";
    }
}

Role role_from_key(const std::string& key) {
    if (key == "encoder") return Role::EncoderOnly;
    if (key == "decoder") return Role::DecoderOnly;
    return Role::Both;
}

namespace {

std::once_flag g_once;
Role           g_role = Role::Both;

void load_once() {
    char* path = obs_module_config_path("role.json");
    if (!path) return;
    obs_data_t* d = obs_data_create_from_json_file(path);
    bfree(path);
    if (!d) return;                       // never set: Both, which is correct
    const char* v = obs_data_get_string(d, "role");
    if (v && *v) g_role = role_from_key(v);
    obs_data_release(d);
}

} // namespace

Role plugin_role() {
    std::call_once(g_once, load_once);
    return g_role;
}

void set_plugin_role(Role r) {
    std::call_once(g_once, load_once);   // so a later read cannot undo this
    g_role = r;

    char* dir = obs_module_config_path("");
    if (dir) { os_mkdirs(dir); bfree(dir); }

    obs_data_t* d = obs_data_create();
    obs_data_set_string(d, "role", role_key(r));
    char* path = obs_module_config_path("role.json");
    if (path) {
        if (!obs_data_save_json_safe(d, path, "tmp", "bak"))
            mlog_warn("could not save the role to %s", path);
        bfree(path);
    }
    obs_data_release(d);
    mlog_info("role set to '%s' — takes effect when OBS restarts", role_key(r));
}

} // namespace multisite_obs
