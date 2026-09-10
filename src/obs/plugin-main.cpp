// SPDX-License-Identifier: GPL-3.0-or-later
#include <obs-module.h>
#include "plugin_log.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multisite", "en-US")

namespace multisite_obs {
void register_output();
void register_source();
void register_ui();
void unregister_ui();
void start_web_ui();
void stop_web_ui();
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
    // Both types are registered whatever role this machine is set to. The
    // role decides which PANELS appear, never which sources exist: a scene
    // collection holding a Multisite Source has to keep resolving it, and a
    // preference is no reason to take a source type away from one.
    multisite_obs::register_output();   // main campus: sends
    multisite_obs::register_source();   // satellite: receives
    multisite_obs::register_ui();       // hotkeys + Tools menu (no Qt needed)
#ifdef MULTISITE_HAVE_QT
    multisite_obs::register_docks();    // encoder + decoder operator panels
#endif
    // The phone-and-tablet interface, served from this process. No Qt either:
    // a build without docks still gets remote control, because the machine that
    // most needs it is the one nobody is sitting at.
    multisite_obs::start_web_ui();
    return true;
}
void obs_module_unload(void) {
    // Stopped first: while it is running, a request can arrive at any moment,
    // and it must not arrive after the things it controls have gone.
    multisite_obs::stop_web_ui();
    multisite_obs::unregister_ui();
    mlog_info("obs-multisite unloaded");
}
