// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// vendor_api.h — the plugin's commands, reachable over obs-websocket.
//
// §8.3 of the scope: the appliance and the plugins expose "the same command
// names with the same payloads", the appliance over HTTP and the plugins over
// obs-websocket vendor requests. This is the plugin's side of that, and it is
// an adapter: every request runs the same function the docks, the hotkeys and
// the remote-control pages already run.
//
namespace multisite_obs {

// Registers this plugin's commands as obs-websocket vendor requests, and its
// state changes as vendor events.
//
// MUST be called from obs_module_post_load(), not obs_module_load(): the
// obs-websocket header is explicit that a vendor registered any earlier is
// never seen.
//
// Safe when obs-websocket is not installed: registration returns null, one line
// is logged, and the plugin carries on with its docks, hotkeys and pages. A
// church that never turns obs-websocket on loses nothing it was already using.
void register_vendor_api();

// Unregisters the requests and stops the event poller. Called from
// obs_module_unload, so nothing is left registered against a module that is
// going away.
void unregister_vendor_api();

// True once a vendor was actually registered. For the log, and so a build can
// assert the surface exists without a running OBS.
bool vendor_api_active();

} // namespace multisite_obs
