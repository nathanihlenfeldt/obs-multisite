// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_dialog.h"

#include "../../core/s3_transport.h"
#include "../../core/storage_manager.h"
#include "../plugin_log.h"

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

#include <algorithm>
#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace multisite_obs {

// How many events are measured at once. Each one is a listing of its own, and a
// store answers several happily; one at a time makes the wait the SUM of every
// event's listing, which is what had this window sitting on "Looking…" for
// minutes on a full bucket.
constexpr int kTallyWorkers = 6;

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

StorageDialog::StorageDialog(QWidget* parent, std::string room_id,
                             multisite::S3Config s3)
    : QDialog(parent), m_room_id(std::move(room_id)), m_s3(std::move(s3)) {
    setWindowTitle(tr_("Storage.Title"));
    setMinimumWidth(520);

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

StorageDialog::~StorageDialog() {
    // Whatever is in flight, stop it: the workers hold their own transport and
    // settings, so setting the gate is enough and they cannot touch this window.
    cancelOperation();
}

void StorageDialog::reject() {
    cancelOperation();
    QDialog::reject();
}

// A fresh gate, transport and manager for one operation. The transport is
// per-operation because cancelling one is one-way: the settings are what can be
// reused, never a cancelled transport.
void StorageDialog::beginOperation() {
    cancelOperation();

    m_cancel   = std::make_shared<std::atomic<bool>>(false);
    m_events   = std::make_shared<std::vector<multisite::ManagedEvent>>();
    m_stats    = std::make_shared<multisite::ListStats>();
    m_op_tx    = std::make_shared<multisite::S3Transport>(m_s3);
    m_mgr      = std::make_shared<multisite::StorageManager>(m_room_id, *m_op_tx);
}

void StorageDialog::cancelOperation() {
    if (m_cancel) m_cancel->store(true);
    if (m_op_tx) m_op_tx->cancel_pending();
    m_listing = false;
}

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
    for (QWidget* w : { (QWidget*)m_refresh, (QWidget*)m_deleteSelected,
                        (QWidget*)m_deleteOlder, (QWidget*)m_close })
        w->setEnabled(!busy);
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

// ── Listing ──────────────────────────────────────────────────────────────────
// Two phases, deliberately. The events appear as soon as their manifests are
// read — names, dates, and which one is on air — and the sizes arrive
// afterwards, each row filling in as it is measured. One pass is what had this
// window sitting on "Looking…" for minutes on a full bucket, with nothing on
// screen to say it was working.
void StorageDialog::onRefresh() {
    beginOperation();
    m_list->clear();
    m_tallied.store(0);
    m_listing = true;
    setBusy(true);
    m_summary->setText(tr_("Storage.Busy"));

    auto events = m_events;
    auto stats  = m_stats;
    auto cancel = m_cancel;
    auto mgr    = m_mgr;

    QPointer<StorageDialog> guard(this);
    std::thread([guard, mgr, events, stats, cancel]() {
        std::string err;
        const bool ok = mgr->list_events(*events, err, stats.get(), cancel.get());
        const qint64 ms = (qint64)stats->elapsed_ms;
        if (guard)
            QMetaObject::invokeMethod(guard.data(),
                [guard, ok, err, ms]() {
                    if (!guard) return;
                    guard->onEventsListed(ok, QString::fromStdString(err), ms);
                }, Qt::QueuedConnection);
    }).detach();
}

void StorageDialog::onEventsListed(bool ok, const QString& error, qint64 ms) {
    if (!ok) {
        m_listing = false;
        setBusy(false);
        m_list->clear();
        m_summary->setText(tr_("Storage.ListFailed").arg(error));
        // The core keeps no log of its own, so this is the only record that a
        // listing was even attempted. Without it, "nothing in the log" means
        // nothing at all.
        mlog_warn("storage: listing failed after %lld ms: %s", (long long)ms,
                  error.toStdString().c_str());
        return;
    }

    mlog_info("storage: listed %d event(s) in %lld ms", (int)m_events->size(),
              (long long)ms);

    drawRows();
    if (m_events->empty()) {
        m_listing = false;
        setBusy(false);
        showSummary();
        return;
    }
    startTallies();
}

// ── Sizes, in parallel ───────────────────────────────────────────────────────
// Each event's tally is a listing of its own, and a store answers several of
// those at once. One at a time makes the wait the sum of every event's listing;
// six at a time makes it the largest, and lets each row fill in as it lands.
void StorageDialog::startTallies() {
    const int n = (int)m_events->size();
    const int workers = std::max(1, std::min(kTallyWorkers, n));

    m_tally_started_ms = QDateTime::currentMSecsSinceEpoch();
    showSummary();

    // The pool's own count, shared by its workers rather than kept in a member:
    // a refresh while this pool is still running gets a new pool with a new
    // counter, so an old worker can never declare the new listing finished.
    auto pool_left = std::make_shared<std::atomic<int>>(workers);
    auto next   = std::make_shared<std::atomic<int>>(0);
    auto events = m_events;
    auto cancel = m_cancel;
    auto mgr    = m_mgr;

    for (int w = 0; w < workers; ++w) {
        // One stats object per worker, merged on the UI thread when the worker
        // is done: the counters are plain integers, and six threads
        // incrementing one struct would be a race.
        auto wstats = std::make_shared<multisite::ListStats>();
        QPointer<StorageDialog> guard(this);
        std::thread([guard, mgr, events, cancel, wstats, pool_left, next, n]() {
            for (;;) {
                if (cancel && cancel->load()) break;
                const int i = next->fetch_add(1);
                if (i >= n) break;

                std::string err;
                if (!mgr->tally_size((*events)[i], err, wstats.get(),
                                     cancel.get()) &&
                    !err.empty() && err != "cancelled") {
                    mlog_warn("storage: could not measure %s: %s",
                              (*events)[i].event_id.c_str(), err.c_str());
                }

                if (guard)
                    QMetaObject::invokeMethod(guard.data(), [guard, i]() {
                        if (!guard) return;
                        guard->onOneTallyDone(i);
                    }, Qt::QueuedConnection);
            }

            if (guard)
                QMetaObject::invokeMethod(guard.data(),
                    [guard, wstats, pool_left]() {
                        if (!guard) return;
                        // Safe to read now: this worker has stopped writing.
                        guard->m_stats->requests += wstats->requests;
                        guard->m_stats->tallies_failed += wstats->tallies_failed;
                        if (pool_left->fetch_sub(1) == 1)
                            guard->onPoolFinished();
                    }, Qt::QueuedConnection);
        }).detach();
    }
}

void StorageDialog::onOneTallyDone(int index) {
    m_tallied.fetch_add(1);
    updateRow(index);
    showSummary();
}

void StorageDialog::onPoolFinished() {
    m_listing = false;
    setBusy(false);
    showSummary();

    const qint64 ms = QDateTime::currentMSecsSinceEpoch() - m_tally_started_ms;
    mlog_info("storage: sizes for %d event(s) in %lld ms "
              "(%d request(s), %d could not be measured)%s",
              (int)m_events->size(), (long long)ms, m_stats->requests,
              m_stats->tallies_failed,
              (m_cancel && m_cancel->load()) ? " [cancelled]" : "");
}

// ── Drawing ──────────────────────────────────────────────────────────────────
QString StorageDialog::rowText(const multisite::ManagedEvent& e) const {
    const QString when = e.started_at_ms > 0
        ? QDateTime::fromMSecsSinceEpoch(e.started_at_ms)
              .toString("ddd d MMM yyyy, HH:mm")
        : QString();
    const QString name = QString::fromStdString(e.name).trimmed();
    const QString title = name.isEmpty() ? when : name;

    QString text = title;
    if (!when.isEmpty() && !name.isEmpty()) text += "   ·   " + when;

    if (e.size_known) {
        text += QString("   ·   %1 · %2")
                    .arg((qulonglong)e.objects)
                    .arg(friendlyBytes(e.bytes));
    } else if (!e.size_error.empty()) {
        // A tally that failed. Saying "0 B" here would tell an operator that an
        // event holding gigabytes is empty.
        text += "   ·   " + tr_("Storage.SizeUnknown");
    } else {
        text += "   ·   " + tr_("Storage.Pending");
    }

    if (e.is_live) text += "   (" + tr_("Storage.Live") + ")";
    return text;
}

void StorageDialog::drawRows() {
    m_list->clear();
    for (const auto& e : *m_events) {
        auto* item = new QListWidgetItem(rowText(e), m_list);
        item->setData(Qt::UserRole, QString::fromStdString(e.event_id));
        item->setData(Qt::UserRole + 1, e.is_live);
    }
}

void StorageDialog::updateRow(int index) {
    if (index < 0 || index >= (int)m_events->size()) return;
    auto* item = m_list->item(index);
    if (item) item->setText(rowText((*m_events)[index]));
}

void StorageDialog::showSummary() {
    const int total = (int)m_events->size();
    uint64_t bytes = 0;
    int measured = 0;
    for (const auto& e : *m_events) {
        if (e.size_known) { bytes += e.bytes; ++measured; }
    }

    if (m_listing && measured < total) {
        m_summary->setText(tr_("Storage.Measuring").arg(measured).arg(total));
        return;
    }
    const int failed = total - measured;
    if (failed > 0) {
        m_summary->setText(tr_("Storage.SizesUnavailable")
                               .arg(total)
                               .arg(friendlyBytes(bytes))
                               .arg(failed));
        return;
    }
    m_summary->setText(tr_("Storage.Summary")
                           .arg(total)
                           .arg(friendlyBytes(bytes)));
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
    auto s3 = m_s3;
    const std::string room = m_room_id;
    runAsync([s3, room, id, rep]() {
        // Its own transport: the listing's may have been cancelled (a refresh,
        // or the window closing), and cancelling a transport is one-way — it
        // would abort this delete's first request, which reads as the store
        // refusing it.
        multisite::S3Transport tx(s3);
        multisite::StorageManager mgr(room, tx);
        *rep = mgr.delete_event(id);
    },
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
    auto s3 = m_s3;
    const std::string room = m_room_id;
    const int64_t now = (int64_t)QDateTime::currentMSecsSinceEpoch();
    runAsync([s3, room, days, now, rep]() {
        multisite::S3Transport tx(s3);          // see onDeleteSelected
        multisite::StorageManager mgr(room, tx);
        *rep = mgr.delete_older_than(days, now);
    },
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

