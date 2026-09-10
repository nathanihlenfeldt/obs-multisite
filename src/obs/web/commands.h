// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// commands.h — the work behind the command names, done once.
//
// The scope is explicit that the obs-websocket API layer is "an adapter over
// what the docks and hotkeys already call — no new control logic, and no second
// path to keep in step". The decoder's controls already satisfy that on their
// own: multisite_ui.h exposes them the way the hotkeys call them, and both
// transports use that. The encoder's do not, because Go Live has to resolve an
// event name and End has to refuse when nothing is on air — so that handful of
// bodies lives here, and the HTTP pages and the vendor requests both call it.
//
// The status and settings documents are here for the same reason: an operator
// watching a phone and an automation reading a vendor response must not be told
// two different things about one machine.
//
#include <string>

namespace multisite_obs {

// ── Status and settings, as JSON text ────────────────────────────────────────

std::string encoder_status_json();
std::string encoder_settings_json();
std::string decoder_status_json();
std::string decoder_events_json();
std::string decoder_settings_json();

// ── Encoder commands ─────────────────────────────────────────────────────────
// These touch the broadcast controller and so must run on OBS's UI thread; the
// caller marshals (see ui_thread.h). Returning false sets `error` to the
// operator-facing reason, in words rather than in a status code.

bool encoder_go_live(const std::string& event_name, std::string& error);
bool encoder_end(std::string& error);
bool encoder_marker(const std::string& label, std::string& error);

// ── Settings ─────────────────────────────────────────────────────────────────
// A partial document changes only what it names. The guard rails live in the
// applier, not in the caller, so no transport can save a figure that would make
// the machine unusable from the very interface being used to fix it.

// The event name is deliberately NOT applied: it is per-event and belongs to Go
// Live, so a settings save must not quietly rename a running broadcast.
bool encoder_apply_settings(const std::string& json_body, std::string& error);
bool decoder_apply_settings(const std::string& json_body, std::string& error);

} // namespace multisite_obs
