// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// encoder_api.h — the main-site half of the remote control, as JSON over HTTP.
//
// Everything the encoder dock offers appears here, and nothing that does not:
// the storage and media settings, Go Live and End, the marker buttons, and the
// reliability readout an operator watches while an event is going out. It is
// registered only when this machine's role includes sending, so a satellite has
// no encoder routes to reach at all.
//
#include "core/http_server.h"

namespace multisite_obs {

void register_encoder_api(multisite::HttpServer& server);

} // namespace multisite_obs