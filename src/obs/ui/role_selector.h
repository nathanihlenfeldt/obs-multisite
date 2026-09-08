// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// role_selector.h — the "what is this machine for" control.
//
// It appears in BOTH dock settings dialogs, and that is not duplication for
// its own sake: choosing a role hides the other dock, so a control that lived
// in only one of them could hide the only way to change it back. Every role
// leaves at least one dock on screen, so putting it in both means an operator
// can always undo the choice.
//
class QGroupBox;
class QWidget;

namespace multisite_obs {

// A group box holding the selector, ready to add to a settings dialog. Saves
// immediately on change and tells the operator a restart is needed, so the
// caller has nothing to wire up.
QGroupBox* make_role_selector(QWidget* parent);

} // namespace multisite_obs
