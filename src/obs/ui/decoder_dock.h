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
class QListWidget;

namespace multisite_obs {

struct DecoderSnapshot;

// Timeline strip: retained window, buffered region, playhead, live edge and
// marker ticks. Clicking seeks.
// Timeline in CLOCK TIME. Shows the recording that still exists in storage,
// which parts are downloaded here, where the playhead is, the live edge, and
// marker ticks. Hovering reports the recorded time under the cursor; clicking
// goes there.
class TimelineBar : public QWidget {
    Q_OBJECT
public:
    explicit TimelineBar(QWidget* parent = nullptr);

    void setSpan(long long earliest_ms, long long live_ms);
    void setPlayhead(long long ms);
    void setDownloaded(std::vector<std::pair<long long, long long>> spans);
    void setMarkers(std::vector<long long> times_ms);

    QSize minimumSizeHint() const override;

signals:
    void seekRequested(long long wall_ms);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    double fraction(long long ms) const;
    long long timeAt(int x) const;

    long long m_earliest = 0, m_live = 0, m_head = 0;
    std::vector<std::pair<long long, long long>> m_downloaded;
    std::vector<long long> m_markers;
    int  m_hoverX = -1;
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
    void onSeek(long long wall_ms);
    void onPlay();
    void onStop();
    void onLockToggled(bool);
    void onJog(double seconds);
    void onGoToDelay();
    void onRefreshEvents();
    void onLoadEvent();
    void onReturnToLive();
    void onEventActivated();

private:
    // Rebuilds the recordings list from the current listing. Separate from
    // refresh() only because that function is already long.
    void refreshEvents(const DecoderSnapshot& s);

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
    QPushButton* m_play = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_lock = nullptr;
    QSpinBox*    m_delayMins = nullptr;
    QPushButton* m_goTo = nullptr;
    QLabel*      m_hint = nullptr;
    QComboBox* m_markers = nullptr;
    QPushButton* m_jumpMarker = nullptr;

    // ── Recordings ───────────────────────────────────────────────────────────
    // The list is rebuilt only when its contents actually change: it refreshes
    // twice a second alongside everything else, and replacing the rows every
    // tick would discard the operator's selection as they reached for Load.
    QListWidget* m_events = nullptr;
    QPushButton* m_refreshEvents = nullptr;
    QPushButton* m_loadEvent = nullptr;
    QPushButton* m_returnLive = nullptr;
    QLabel*      m_eventsNote = nullptr;
    QLabel*      m_liveElsewhere = nullptr;
    QString      m_events_signature;
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
    QSpinBox*  m_bufferMins = nullptr;
    // In a dialog rather than the dock, for the same reason as the encoder:
    // settings are set once, the dock is watched mid-service.
    QDialog* m_settings = nullptr;
    QPushButton* m_settingsBtn = nullptr;
};

} // namespace multisite_obs
