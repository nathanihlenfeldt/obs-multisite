// SPDX-License-Identifier: GPL-3.0-or-later
#include "role_selector.h"
#include "../plugin_role.h"

#include <obs-module.h>

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>

namespace multisite_obs {

namespace {

QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

} // namespace

QGroupBox* make_role_selector(QWidget* parent) {
    auto* box  = new QGroupBox(tr_("Dock.Role"), parent);
    auto* form = new QFormLayout(box);

    auto* combo = new QComboBox(box);
    combo->addItem(tr_("Dock.RoleBoth"),    QString("both"));
    combo->addItem(tr_("Dock.RoleEncoder"), QString("encoder"));
    combo->addItem(tr_("Dock.RoleDecoder"), QString("decoder"));

    const int idx = combo->findData(QString(role_key(plugin_role())));
    if (idx >= 0) combo->setCurrentIndex(idx);

    auto* note = new QLabel(box);
    note->setWordWrap(true);
    note->setText(tr_("Dock.RoleHint"));

    QObject::connect(combo, &QComboBox::currentIndexChanged, box,
                     [combo, note](int) {
        set_plugin_role(role_from_key(combo->currentData().toString().toStdString()));
        // The docks are created while the module loads, so nothing can move
        // now. Say that plainly instead of letting somebody conclude the
        // setting does not work.
        note->setText(tr_("Dock.RoleRestart"));
    });

    form->addRow(tr_("Dock.RoleThisMachine"), combo);
    form->addRow(QString(), note);
    return box;
}

} // namespace multisite_obs
