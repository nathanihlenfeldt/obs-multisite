#pragma once
//
// decoder_dock.h — the satellite operator panel.
//
// The DVR surface from the design: room state, how far behind live, a timeline
// showing the retained window / buffered content / playhead / markers, and
// Pause / Resume / Jump-to-live.
//
#include <QWidget>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QPushButton;
class QComboBox;
class QTimer;
class QLineEdit;
class QSpinBox;
class QDialog;

namespace multisite_obs {

// Timeline strip: retained window, buffered region, playhead, live edge and
// marker ticks. Clicking seeks.
class TimelineBar : public QWidget {
    Q_OBJECT
public:
    explicit TimelineBar(QWidget* parent = nullptr);

    void setRange(unsigned long long first, unsigned long long live);
    void setHead(unsigned long long head);
    void setBufferedTo(unsigned long long seq);
    void setMarkers(std::vector<unsigned long long> seqs);

    QSize minimumSizeHint() const override;

signals:
    void seekRequested(unsigned long long seq);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    double fraction(unsigned long long seq) const;

    unsigned long long m_first = 0, m_live = 0, m_head = 0, m_buffered = 0;
    std::vector<unsigned long long> m_markers;
};

class DecoderDock : public QWidget {
    Q_OBJECT
public:
    explicit DecoderDock(QWidget* parent = nullptr);

private slots:
    void refresh();
    void onStart();
    void onSaveSettings();
    void onOpenSettings();
    void onPause();
    void onResume();
    void onJumpLive();
    void onJumpMarker();
    void onSeek(unsigned long long seq);

private:
    QLabel* m_room = nullptr;
    QLabel* m_state = nullptr;
    QLabel* m_behind = nullptr;
    QLabel* m_buffered = nullptr;
    QLabel* m_cached = nullptr;
    QLabel* m_marker = nullptr;
    QLabel* m_audio = nullptr;
    QLabel* m_error = nullptr;
    TimelineBar* m_timeline = nullptr;
    QPushButton* m_pause = nullptr;
    QPushButton* m_resume = nullptr;
    QPushButton* m_live = nullptr;
    QPushButton* m_start = nullptr;
    QComboBox* m_markers = nullptr;
    QPushButton* m_jumpMarker = nullptr;
    QTimer* m_timer = nullptr;
    size_t m_marker_count = 0;

    // Machine-wide storage settings, entered once here.
    QLineEdit* m_accountId = nullptr;
    QLineEdit* m_endpoint = nullptr;
    QLineEdit* m_bucket = nullptr;
    QLineEdit* m_keyId = nullptr;
    QLineEdit* m_secret = nullptr;
    QLineEdit* m_region = nullptr;
    QLineEdit* m_roomId = nullptr;
    QSpinBox*  m_prebuffer = nullptr;
    // In a dialog rather than the dock, for the same reason as the encoder:
    // settings are set once, the dock is watched mid-service.
    QDialog* m_settings = nullptr;
    QPushButton* m_settingsBtn = nullptr;
};

} // namespace multisite_obs
