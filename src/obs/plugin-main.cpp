#include <obs-module.h>
#include "plugin_log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multisite", "en-US")

namespace multisite_obs {
void register_output();
void register_source();
void register_ui();
void unregister_ui();
}

MODULE_EXPORT const char* obs_module_description(void) {
    return "Multisite: reliable store-and-forward video contribution over "
           "S3-compatible storage.";
}
MODULE_EXPORT const char* obs_module_name(void) { return "Multisite"; }

bool obs_module_load(void) {
    mlog_info("loading obs-multisite %s", PLUGIN_VERSION);
    multisite_obs::register_output();   // main campus: sends
    multisite_obs::register_source();   // satellite: receives
    multisite_obs::register_ui();       // hotkeys + Tools menu (no Qt needed)
    return true;
}
void obs_module_unload(void) {
    multisite_obs::unregister_ui();
    mlog_info("obs-multisite unloaded");
}
