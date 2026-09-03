// dock_register.cpp — installs the two docks in the OBS window.
//
// Compiled only when the plugin is built with Qt and obs-frontend-api
// (ENABLE_QT). Everything else in the plugin works without them.
#include <obs-module.h>
#include <obs-frontend-api.h>

#include "encoder_dock.h"
#include "decoder_dock.h"
#include "../plugin_log.h"

namespace multisite_obs {

void register_docks() {
    // add_dock_by_id takes ownership of the widget and remembers its geometry
    // and visibility between sessions, so an operator's layout persists.
    obs_frontend_add_dock_by_id("multisite_encoder",
                                obs_module_text("Dock.Encoder"),
                                new EncoderDock());
    obs_frontend_add_dock_by_id("multisite_decoder",
                                obs_module_text("Dock.Decoder"),
                                new DecoderDock());
    mlog_info("registered encoder and decoder docks");
}

} // namespace multisite_obs
