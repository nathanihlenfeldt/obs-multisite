// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_dialog.h"

#include "../../core/s3_transport.h"
#include "../../core/storage_manager.h"

#include <obs-module.h>

#include <QAbstractItemView>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <thread>
#include <utility>
#include <vector>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

StorageDialog::StorageDialog(QWidget* parent, std::string room_id,
                             std::shared_ptr<multisite::S3Transport> tx)
    : QDialog(parent), m_room_id(std::move(room_id)), m_tx(std::move(tx)) {
    setWindowTitle(tr_("Storage.Title"));
    setMinimumWidth(520);

    m_mgr = std::make_shared<multisite::StorageManager>(m_room_id, *m_tx);

    auto* root = new QVBoxLayout(this);
    m_summary = new QLabel(tr_("Storage.Busy"), this);
    root->addWidget(m_summary);

    m_list = new QListWidget(this);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    root->addWidget(m_list, 1);

    auto* row = new QHBoxLayout();
    m_refresh = new QPushButton(tr_("Storage.Refresh"), this);
    m_deleteSelected = new QPushButton(tr_("Storage.DeleteSelected"), this);
    row->addWidget(m_refresh);
    row->addWidget(m_deleteSelected);
    row->addStretch(1);
    root->addLayout(row);

    auto* olderRow = new QHBoxLayout();
    m_deleteOlder = new QPushButton(tr_("Storage.DeleteOlder"), this);
    m_olderDays = new QSpinBox(this);
    m_olderDays->setRange(1, 365);
    m_olderDays->setValue(7);
    m_olderDays->setSuffix(tr_("Storage.Days"));
    olderRow->addWidget(m_deleteOlder);
    olderRow->addWidget(m_olderDays);
    olderRow->addStretch(1);
    root->addLayout(olderRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_close = buttons->button(QDialogButtonBox::Close);
    root->addWidget(buttons);

    connect(m_refresh, &QPushButton::clicked, this, &StorageDialog::onRefresh);
    connect(m_deleteSelected, &QPushButton::clicked, this, &StorageDialog::onDeleteSelected);
    connect(m_deleteOlder, &QPushButton::clicked, this, &StorageDialog::onDeleteOlder);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        m_deleteSelected->setEnabled(!selectedEventId().empty());
    });
    m_deleteSelected->setEnabled(false);

    onRefresh();
}

StorageDialog::~StorageDialog() = default;

void StorageDialog::runAsync(std::function<void()> work, std::function<void()> done) {
    setBusy(true);
    QPointer<StorageDialog> guard(this);
    std::thread([guard, work = std::move(work), done = std::move(done)]() mutable {
        work();
        if (guard)
            QMetaObject::invokeMethod(guard.data(),
                [guard, done = std::move(done)]() mutable {
                    if (!guard) return;
                    guard->setBusy(false);
                    done();
                }, Qt::QueuedConnection);
    }).detach();
}

void StorageDialog::setBusy(bool busy) {
    m_busy = busy;
    for (QWidget* w : { (QWidget*)m_refresh, (QWidget*)m_deleteSelected,
                        (QWidget*)m_deleteOlder, (QWidget*)m_close })
        w->setEnabled(!busy);
    if (busy) m_summary->setText(tr_("Storage.Busy"));
}

QString StorageDialog::friendlyBytes(uint64_t bytes) const {
    if (bytes >= 1'000'000'000ull)
        return QString::number(bytes / 1e9, 'f', 1) + " GB";
    if (bytes >= 1'000'000ull)
        return QString::number(bytes / 1e6, 'f', 1) + " MB";
    if (bytes >= 1000ull)
        return QString::number(bytes / 1000.0, 'f', 0) + " KB";
    return QString::number(bytes) + " B";
}

std::string StorageDialog::selectedEventId() const {
    auto* item = m_list->currentItem();
    if (!item) return "";
    return item->data(Qt::UserRole).toString().toStdString();
}

void StorageDialog::onRefresh() {
    auto events = std::make_shared<std::vector<multisite::ManagedEvent>>();
    auto err = std::make_shared<std::string>();
    auto mgr = m_mgr;
    runAsync([mgr, events, err]() {
        if (!mgr->list(*events, *err) && err->empty())
            *err = "listing failed";
    }, [this, events, err]() {
        if (!err->empty()) {
            m_summary->setText(tr_("Storage.ListFailed")
                                   .arg(QString::fromStdString(*err)));
            m_list->clear();
            return;
        }
        m_list->clear();
        uint64_t total = 0;
        for (const auto& e : *events) {
            total += e.bytes;
            const QString when = e.started_at_ms > 0
                ? QDateTime::fromMSecsSinceEpoch(e.started_at_ms)
                      .toString("ddd d MMM yyyy, HH:mm")
                : QString();
            const QString name = QString::fromStdString(e.name).trimmed();
            const QString title = name.isEmpty() ? when : name;
            QString text = title;
            if (!when.isEmpty() && !name.isEmpty()) text += "   ·   " + when;
            text += QString("   ·   %1 · %2")
                        .arg((qulonglong)e.objects)
                        .arg(friendlyBytes(e.bytes));
            if (e.is_live) text += "   (" + tr_("Storage.Live") + ")";
            auto* item = new QListWidgetItem(text, m_list);
            item->setData(Qt::UserRole, QString::fromStdString(e.event_id));
            item->setData(Qt::UserRole + 1, e.is_live);
        }
        m_summary->setText(tr_("Storage.Summary")
                               .arg((qulonglong)events->size())
                               .arg(friendlyBytes(total)));
    });
}

void StorageDialog::onDeleteSelected() {
    const std::string id = selectedEventId();
    if (id.empty()) return;

    auto* item = m_list->currentItem();
    if (item && item->data(Qt::UserRole + 1).toBool()) {
        QMessageBox::information(this, tr_("Storage.Title"), tr_("Storage.LiveBlocked"));
        return;
    }

    const QString label = item ? item->text() : QString::fromStdString(id);
    if (QMessageBox::question(this, tr_("Storage.Title"),
                              tr_("Storage.ConfirmDelete").arg(label),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    auto rep = std::make_shared<multisite::DeleteReport>();
    auto mgr = m_mgr;
    runAsync([mgr, id, rep]() { *rep = mgr->delete_event(id); },
             [this, rep]() {
                 if (rep->ok) {
                     QString done = tr_("Storage.Deleted")
                                        .arg((qulonglong)rep->objects_deleted)
                                        .arg(friendlyBytes(rep->bytes_freed));
                     if (!rep->confirmed) done += " " + tr_("Storage.NotConfirmed");
                     QMessageBox::information(this, tr_("Storage.Title"), done);
                 } else {
                     QMessageBox::warning(this, tr_("Storage.Title"),
                                          QString::fromStdString(rep->error));
                 }
                 onRefresh();
             });
}

void StorageDialog::onDeleteOlder() {
    const int days = m_olderDays->value();
    if (QMessageBox::question(this, tr_("Storage.Title"),
                              tr_("Storage.ConfirmOlder").arg(days),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    auto rep = std::make_shared<multisite::DeleteReport>();
    auto mgr = m_mgr;
    const int64_t now = (int64_t)QDateTime::currentMSecsSinceEpoch();
    runAsync([mgr, days, now, rep]() { *rep = mgr->delete_older_than(days, now); },
             [this, rep]() {
                 if (rep->ok) {
                     QMessageBox::information(this, tr_("Storage.Title"),
                                              tr_("Storage.Deleted")
                                                  .arg((qulonglong)rep->objects_deleted)
                                                  .arg(friendlyBytes(rep->bytes_freed)));
                 } else {
                     QMessageBox::warning(this, tr_("Storage.Title"),
                                          QString::fromStdString(rep->error));
                 }
                 onRefresh();
             });
}

} // namespace multisite_obs

