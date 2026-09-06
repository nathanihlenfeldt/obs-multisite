#pragma once
//
// api.h — the browser's view of the relay.
//
// Reuses the appliance's small HTTP server rather than pulling in a web
// framework, for the same reason it exists there: this has to build with one
// command and keep working for years without anyone updating a dependency
// tree.
//
#include "http_server.h"
#include "service.h"

namespace multisite_relay {

void register_routes(multisite_player::HttpServer& server, Service& service);

} // namespace multisite_relay
