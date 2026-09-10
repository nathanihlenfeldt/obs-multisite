// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_dialog.h — the encoder's storage-management surface: list the room's
// events with their sizes, and delete them (one or older-than-N) with a
// confirmation and a verification pass.
//
#include <QDialog>

#include "../../core/s3_transport.h"     // multisite::S3Config, multisite::S3Transport
#include "../../core/storage_manager.h"  // the events and stats are held whole, not by pointer

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace multisite_obs {

class StorageDialog : public QDialog {
    Q_OBJECT
public:
    // Takes the settings rather than a ready-made transport, because every
    // operation builds its own: cancelling a transport is one-way, so a
    // cancelled one must never be reused — a delete through a cancelled
    // transport would abort on its first request and look like a refusal.
    StorageDialog(QWidget* parent, std::string room_id, multisite::S3Config s3);
    ~StorageDialog() override;

protected:
    // Close, Esc and the window's own close button all end here. Stopping the
    // work first is what keeps a closed window from going on hammering the
    // bucket for minutes.
    void reject() override;

private slots:
    void onRefresh();
    void onDeleteSelected();
    void onDeleteOlder();

private:
    // ── One listing ─────────────────────────────────────────────────────────
    // Each refresh gets its own gate, transport and manager, all owned through
    // shared_ptr so a worker that outlives the refresh (or the window) is
    // harmless and, when the gate is set, short-lived.
    void beginOperation();
    void cancelOperation();
    void onEventsListed(bool ok, const QString& error, qint64 ms);
    void startTallies();
    void onOneTallyDone(int index);
    void onPoolFinished();

    void drawRows();
    void updateRow(int index);
    QString rowText(const multisite::ManagedEvent& e) const;
    void showSummary();

    // Run `work` on a worker thread (network I/O must not block the OBS UI
    // thread), then `done` back on the UI thread.
    void runAsync(std::function<void()> work, std::function<void()> done);
    void setBusy(bool busy);
    std::string selectedEventId() const;
    QString friendlyBytes(uint64_t bytes) const;

    std::string m_room_id;
    multisite::S3Config m_s3;

    // The operation in flight, if any.
    std::shared_ptr<std::atomic<bool>>            m_cancel;
    std::shared_ptr<multisite::S3Transport>       m_op_tx;
    std::shared_ptr<multisite::StorageManager>    m_mgr;
    std::shared_ptr<std::vector<multisite::ManagedEvent>> m_events;
    std::shared_ptr<multisite::ListStats>         m_stats;

    std::atomic<int>  m_tallied{0};      // events measured so far this listing
    int64_t           m_tally_started_ms = 0;
    bool              m_listing = false; // between "Looking…" and the last worker

    QLabel*      m_summary = nullptr;
    QListWidget* m_list = nullptr;
    QSpinBox*    m_olderDays = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_deleteSelected = nullptr;
    QPushButton* m_deleteOlder = nullptr;
    QPushButton* m_close = nullptr;
};

} // namespace multisite_obs
