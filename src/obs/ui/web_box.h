// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// web_box.h — the "remote control" group both docks show.
//
// Shared between the encoder and the decoder for the same reason the role
// selector is: a setting that exists in only one panel is a setting somebody
// will spend an afternoon looking for in the other. What it configures is the
// page served on the church network, and the address to type into a phone is
// the one thing an operator actually needs from it.
//
class QGroupBox;
class QWidget;

namespace multisite_obs {

QGroupBox* make_remote_control_box(QWidget* parent);

} // namespace multisite_obs