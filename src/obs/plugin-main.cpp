#include <obs-module.h>
#include "plugin_log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multisite", "en-US")

namespace multisite_obs {
void register_output();
void register_source();
void register_ui();
void unregister_ui();
#ifdef MULTISITE_HAVE_QT
void register_docks();
#endif
}

MODULE_EXPORT const char* obs_module_description(void) {
    return "Multisite: reliable store-and-forward video contribution over "
           "S3-compatible storage.";
}
MODULE_EXPORT const char* obs_module_name(void) { return "Multisite"; }

bool obs_module_load(void) {
#ifdef MULTISITE_HAVE_QT
    mlog_info("loading obs-multisite %s (with operator docks)", PLUGIN_VERSION);
#else
    mlog_info("loading obs-multisite %s (hotkeys only — this build has no "
              "operator docks)", PLUGIN_VERSION);
#endif
    multisite_obs::register_output();   // main campus: sends
    multisite_obs::register_source();   // satellite: receives
    multisite_obs::register_ui();       // hotkeys + Tools menu (no Qt needed)
#ifdef MULTISITE_HAVE_QT
    multisite_obs::register_docks();    // encoder + decoder operator panels
#endif
    return true;
}
void obs_module_unload(void) {
    multisite_obs::unregister_ui();
    mlog_info("obs-multisite unloaded");
}
