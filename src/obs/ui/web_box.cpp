// SPDX-License-Identifier: GPL-3.0-or-later
#include "web_box.h"

#include "../web/web_ui.h"

#include <obs-module.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>

#include <string>

namespace multisite_obs {

namespace {

QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

} // namespace

QGroupBox* make_remote_control_box(QWidget* parent) {
    auto* box  = new QGroupBox(tr_("Dock.RemoteBox"), parent);
    auto* form = new QFormLayout(box);

    auto* on = new QCheckBox(tr_("Dock.RemoteOn"), box);
    on->setChecked(web_ui_settings().enabled);

    auto* port = new QSpinBox(box);
    port->setRange(1024, 65535);
    port->setValue(web_ui_settings().port);

    auto* address = new QLabel(box);
    // Selectable, because the whole point of it is that somebody types it into
    // another device.
    address->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* note = new QLabel(tr_("Dock.RemoteHint"), box);
    note->setWordWrap(true);
    note->setStyleSheet("color: palette(text); opacity: 0.75;");

    auto show_address = [address]() {
        address->setText(web_ui_settings().enabled
                             ? QString::fromStdString(web_ui_address())
                             : tr_("Dock.RemoteOff"));
    };
    show_address();

    // Applied at once rather than at the next start: somebody setting this up
    // wants to try it from their phone now, not to restart OBS to find out
    // whether it worked.
    auto apply = [on, port, address, show_address]() {
        WebUiSettings s = web_ui_settings();
        const bool was_on   = s.enabled;
        const int  was_port = s.port;
        s.enabled = on->isChecked();
        s.port    = port->value();

        // Nothing actually changed, so leave the server alone. The port field
        // reports editingFinished the moment it loses focus — which is a click
        // on any other control — and restarting the server on that would drop
        // the page somebody is holding, for no reason at all.
        if (s.enabled == was_on && s.port == was_port) {
            show_address();
            return;
        }

        set_web_ui_settings(s);

        stop_web_ui();
        start_web_ui();
        show_address();

        const std::string problem = web_ui_problem();
        if (!problem.empty())
            address->setText(tr_("Dock.RemoteFailed")
                                 .arg(QString::fromStdString(problem)));
    };

    QObject::connect(on, &QCheckBox::toggled, box,
                     [apply](bool) { apply(); });
    QObject::connect(port, &QSpinBox::editingFinished, box,
                     [apply]() { apply(); });

    form->addRow(QString(), on);
    form->addRow(tr_("Dock.RemotePort"), port);
    form->addRow(tr_("Dock.RemoteAddress"), address);
    form->addRow(QString(), note);
    return box;
}

} // namespace multisite_obs