// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_dialog.h — the encoder's storage-management surface: list the room's
// events with their sizes, and delete them (one or older-than-N) with a
// confirmation and a verification pass.
//
#include <QDialog>

#include <functional>
#include <memory>
#include <string>

class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace multisite {
class S3Transport;
class StorageManager;
}

namespace multisite_obs {

class StorageDialog : public QDialog {
    Q_OBJECT
public:
    StorageDialog(QWidget* parent, std::string room_id,
                  std::shared_ptr<multisite::S3Transport> tx);
    ~StorageDialog() override;

private slots:
    void onRefresh();
    void onDeleteSelected();
    void onDeleteOlder();

private:
    // Run `work` on a worker thread (network I/O must not block the OBS UI
    // thread), then `done` back on the UI thread. The transport is shared so a
    // delete keeps going even if the dialog is closed.
    void runAsync(std::function<void()> work, std::function<void()> done);
    void setBusy(bool busy);
    std::string selectedEventId() const;
    QString friendlyBytes(uint64_t bytes) const;

    std::string m_room_id;
    std::shared_ptr<multisite::S3Transport>    m_tx;
    std::shared_ptr<multisite::StorageManager> m_mgr;
    bool m_busy = false;

    QLabel*      m_summary = nullptr;
    QListWidget* m_list = nullptr;
    QSpinBox*    m_olderDays = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_deleteSelected = nullptr;
    QPushButton* m_deleteOlder = nullptr;
    QPushButton* m_close = nullptr;
};

} // namespace multisite_obs
