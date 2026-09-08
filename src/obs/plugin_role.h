// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// plugin_role.h — which half of the plugin this machine is for.
//
// Both roles ship in one module, which is what lets any laptop originate a
// broadcast. The cost is that every machine gets both sets of controls, and a
// campus that only ever receives has an encoder dock it must learn to ignore —
// on the same screen as a volunteer who is one wrong click from broadcasting.
//
// So the docks can be limited to one role. Deliberately the DOCKS only: the
// output and source types stay registered whatever the setting says, because a
// scene collection that already contains a Multisite Source must keep
// resolving it. Hiding a panel is reversible from the interface; unregistering
// a source type that a church's scene collection depends on is not the sort of
// thing to do on a preference.
//
#include <string>

namespace multisite_obs {

enum class Role {
    Both,          // the default, and what any machine that might do either wants
    EncoderOnly,   // a main site: no decoder dock
    DecoderOnly,   // a satellite: no encoder dock
};

// Read once from the module's own config, at load. Never fails: an absent or
// unreadable file means Both.
Role plugin_role();

// Persist and update. Takes effect at the next OBS start, because docks are
// registered during module load; callers say so rather than leaving somebody
// wondering why nothing changed.
void set_plugin_role(Role r);

const char* role_key(Role r);      // the stored form: "both"/"encoder"/"decoder"
Role role_from_key(const std::string& key);

} // namespace multisite_obs
