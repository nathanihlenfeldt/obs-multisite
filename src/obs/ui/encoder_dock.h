#pragma once
//
// encoder_dock.h — the main campus operator panel.
//
// Replaces the Lua control script: storage settings, Go Live / End, a live
// reliability readout (queue depth, retries, link health) and marker buttons,
// all in a dock OBS remembers the position of.
//
#include <QWidget>

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;
class QTimer;
class QCheckBox;

namespace multisite_obs {

class EncoderDock : public QWidget {
    Q_OBJECT
public:
    explicit EncoderDock(QWidget* parent = nullptr);

private slots:
    void onGoLive();
    void onEnd();
    void onSaveSettings();
    void onMarker(int index);
    void refresh();

private:
    void loadIntoFields();
    void setLiveState(bool live);

    // storage
    QLineEdit* m_accountId = nullptr;
    QLineEdit* m_endpoint = nullptr;
    QLineEdit* m_bucket = nullptr;
    QLineEdit* m_keyId = nullptr;
    QLineEdit* m_secret = nullptr;
    QLineEdit* m_region = nullptr;
    QLineEdit* m_room = nullptr;
    QCheckBox* m_tags = nullptr;

    // media
    QDoubleSpinBox* m_segDur = nullptr;
    QSpinBox* m_vBitrate = nullptr;
    QSpinBox* m_aBitrate = nullptr;
    QSpinBox* m_tracks = nullptr;
    QLineEdit* m_trackLabels = nullptr;
    QLineEdit* m_channelLabels = nullptr;
    QLineEdit* m_markerLabels = nullptr;

    // controls + status
    QPushButton* m_goLive = nullptr;
    QPushButton* m_end = nullptr;
    QPushButton* m_markers[4] = { nullptr, nullptr, nullptr, nullptr };
    QLabel* m_state = nullptr;
    QLabel* m_uptime = nullptr;
    QLabel* m_confirmed = nullptr;
    QLabel* m_queue = nullptr;
    QLabel* m_retries = nullptr;
    QLabel* m_data = nullptr;
    QLabel* m_link = nullptr;
    QLabel* m_error = nullptr;
    QTimer* m_timer = nullptr;
};

} // namespace multisite_obs
