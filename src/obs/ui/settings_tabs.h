// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// settings_tabs.h — shared shape for both dock settings dialogs.
//
// Both dialogs grew the same way and hit the same wall: enough settings to be
// taller than a laptop screen, at which point the buttons fall off the bottom
// and there is no way to dismiss the window at all. Scrolling fixed that but
// left an operator panning a long strip to find one field.
//
// Tabs are the answer instead, because these settings genuinely fall into
// groups an operator thinks about separately — where the video goes, how it is
// encoded, what this machine is. Each page is short enough to fit without
// scrolling on any normal display, and short pages need no hunting.
//
// The scrolling is kept per page rather than thrown away: it costs nothing
// when the page fits, and on a very small display it is the difference between
// awkward and unusable.
//
#include <QString>

class QWidget;
class QTabWidget;

namespace multisite_obs {

// Wraps `content` in a scroll area and adds it to `tabs` under `title`.
// `content` is reparented, so the caller can build it against any parent.
//
// Compact by design: a settings form is a list of short rows, and Qt's default
// spacing on a form inside a group box inside a tab stacks three sets of
// margins into a surprising amount of nothing.
void add_settings_tab(QTabWidget* tabs, QWidget* content, const QString& title);

// Applies the tightened spacing to a form or box layout and everything under
// it. Called by add_settings_tab; exposed for a page that builds its own
// layout and wants to match.
void tighten(QWidget* w);

} // namespace multisite_obs
