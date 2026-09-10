// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// decoder_api.h — the satellite half of the remote control, as JSON over HTTP.
//
// This is the same job the campus player's own web UI does, from inside OBS:
// play, hold, catch up, jog, stay behind live, pick a past event, and see why
// the picture is late. The controls it calls are the Qt-free ones the hotkeys
// already use, so a page and a keypress cannot disagree about what they did.
//
// Registered only when this machine's role includes receiving, so a main site
// has no decoder routes to reach at all.
//
#include "core/http_server.h"

namespace multisite_obs {

void register_decoder_api(multisite::HttpServer& server);

} // namespace multisite_obs