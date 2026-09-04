#include "decoder_dock.h"

#include "../multisite_ui.h"
#include "../decoder_settings.h"
#include "../plugin_log.h"

#include <obs-module.h>

#include <QComboBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

// Plain-language duration. Volunteers read "1 min 30 sec", not "90 s" and
// certainly not a count of segments.
static QString friendly_duration(double seconds) {
    if (seconds < 1.0) return QObject::tr("none");
    const int total = (int)(seconds + 0.5);
    const int mins = total / 60;
    const int secs = total % 60;
    if (mins == 0) return QObject::tr("%1 sec").arg(secs);
    if (secs == 0) return QObject::tr("%1 min").arg(mins);
    return QObject::tr("%1 min %2 sec").arg(mins).arg(secs);
}

// Clock time of a position in the service, e.g. "10:42:06".
static QString clock_time(long long ms) {
    if (ms <= 0) return QString("--:--");
    return QDateTime::fromMSecsSinceEpoch((qint64)ms).toString("HH:mm:ss");
}

// ── TimelineBar ──────────────────────────────────────────────────────────────
TimelineBar::TimelineBar(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr_("Dock.TimelineHint"));
}

QSize TimelineBar::minimumSizeHint() const { return QSize(120, 26); }

void TimelineBar::setRange(unsigned long long first, unsigned long long live) {
    if (m_first == first && m_live == live) return;
    m_first = first; m_live = live;
    update();
}
void TimelineBar::setHead(unsigned long long head) {
    if (m_head == head) return;
    m_head = head; update();
}
void TimelineBar::setBufferedTo(unsigned long long seq) {
    if (m_buffered == seq) return;
    m_buffered = seq; update();
}
void TimelineBar::setMarkers(std::vector<unsigned long long> seqs) {
    if (m_markers == seqs) return;
    m_markers = std::move(seqs); update();
}

double TimelineBar::fraction(unsigned long long seq) const {
    if (m_live <= m_first) return 0.0;
    if (seq <= m_first) return 0.0;
    if (seq >= m_live) return 1.0;
    return (double)(seq - m_first) / (double)(m_live - m_first);
}

void TimelineBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int h = 8;
    const int y = (height() - h) / 2;
    const int w = width();

    // retained window
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x36, 0x3b, 0x41));
    p.drawRoundedRect(QRect(0, y, w, h), 4, 4);

    if (m_live > m_first) {
        // played region, up to the head
        const int headX = (int)(fraction(m_head) * w);
        p.setBrush(QColor(0x2f, 0x3f, 0x4d));
        p.drawRoundedRect(QRect(0, y, headX, h), 4, 4);

        // buffered ahead of the head
        const int bufX = (int)(fraction(m_buffered) * w);
        if (bufX > headX) {
            p.setBrush(QColor(0x2f, 0x6f, 0x57));
            p.drawRect(QRect(headX, y, bufX - headX, h));
        }

        // marker ticks
        p.setPen(QPen(QColor(0xe0, 0xa0, 0x20), 2));
        for (unsigned long long s : m_markers) {
            const int x = (int)(fraction(s) * w);
            p.drawLine(x, y - 3, x, y + h + 3);
        }

        // live edge
        p.setPen(QPen(QColor(0xe5, 0x48, 0x4d), 2));
        p.drawLine(w - 1, y - 4, w - 1, y + h + 4);

        // playhead
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xdf, 0xe3, 0xe7));
        p.drawEllipse(QPoint(headX, y + h / 2), 6, 6);
        p.setPen(QPen(QColor(0x3b, 0x82, 0xc4), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPoint(headX, y + h / 2), 6, 6);
    }
}

void TimelineBar::mousePressEvent(QMouseEvent* e) {
    if (m_live <= m_first || width() <= 0) return;
    const double f = std::min(1.0, std::max(0.0, (double)e->pos().x() / width()));
    const auto span = m_live - m_first;
    const auto target = m_first + (unsigned long long)(f * (double)span);
    emit seekRequested(target);
}

// ── DecoderDock ──────────────────────────────────────────────────────────────
DecoderDock::DecoderDock(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // header: room + state
    auto* head = new QHBoxLayout();
    m_room = new QLabel(tr_("Dock.NoSource"), this);
    m_room->setStyleSheet("font-weight: bold;");
    m_state = new QLabel(QString(), this);
    head->addWidget(m_room);
    head->addStretch(1);
    head->addWidget(m_state);
    root->addLayout(head);

    // behind-live readout: the number an operator watches
    m_behind = new QLabel("—", this);
    m_behind->setStyleSheet("font-size: 20px; font-weight: 500;");
    root->addWidget(m_behind);

    // timeline
    m_timeline = new TimelineBar(this);
    root->addWidget(m_timeline);
    connect(m_timeline, &TimelineBar::seekRequested,
            this, &DecoderDock::onSeek);

    // controls
    auto* startRow = new QHBoxLayout();
    m_start = new QPushButton(tr_("Dock.Start"), this);
    m_start->setToolTip(tr_("Dock.StartHint"));
    startRow->addWidget(m_start);
    root->addLayout(startRow);
    connect(m_start, &QPushButton::clicked, this, &DecoderDock::onStart);

    auto* row = new QHBoxLayout();
    m_pause  = new QPushButton(tr_("Pause"), this);
    m_resume = new QPushButton(tr_("Resume"), this);
    m_live   = new QPushButton(tr_("JumpToLive"), this);
    row->addWidget(m_pause);
    row->addWidget(m_resume);
    row->addWidget(m_live);
    root->addLayout(row);
    connect(m_pause,  &QPushButton::clicked, this, &DecoderDock::onPause);
    connect(m_resume, &QPushButton::clicked, this, &DecoderDock::onResume);
    connect(m_live,   &QPushButton::clicked, this, &DecoderDock::onJumpLive);

    // markers
    auto* mrow = new QHBoxLayout();
    m_markers = new QComboBox(this);
    m_markers->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_jumpMarker = new QPushButton(tr_("Dock.Jump"), this);
    mrow->addWidget(m_markers, 1);
    mrow->addWidget(m_jumpMarker);
    root->addLayout(mrow);
    connect(m_jumpMarker, &QPushButton::clicked,
            this, &DecoderDock::onJumpMarker);

    // detail
    auto* box = new QGroupBox(tr_("Dock.Status"), this);
    auto* grid = new QGridLayout(box);
    auto addStat = [&](int r, int c, const char* key, QLabel*& out) {
        auto* cap = new QLabel(tr_(key), box);
        // Dim but still legible: palette(mid) is nearly invisible on
        // OBS's dark theme, which left the numbers looking unlabelled.
        cap->setStyleSheet("color: palette(text); opacity: 0.75;");
        out = new QLabel("—", box);
        grid->addWidget(cap, r, c * 2);
        grid->addWidget(out, r, c * 2 + 1);
    };
    addStat(0, 0, "Dock.Buffered", m_buffered);
    addStat(0, 1, "Dock.CanRewind", m_cached);
    addStat(1, 0, "Dock.Marker",   m_marker);
    addStat(1, 1, "Dock.Audio",    m_audio);
    m_error = new QLabel(QString(), box);
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color: #e5484d;");
    m_error->hide();
    grid->addWidget(m_error, 2, 0, 1, 4);
    root->addWidget(box);

    // ── Storage: entered once for this machine ──────────────────────────────
    // Previously these lived only in each source's settings, so they were lost
    // if OBS exited uncleanly and had to be retyped for every source.
    m_settingsBtn = new QPushButton(tr_("Dock.Settings"), this);
    root->addWidget(m_settingsBtn);
    connect(m_settingsBtn, &QPushButton::clicked,
            this, &DecoderDock::onOpenSettings);

    m_settings = new QDialog(this);
    m_settings->setWindowTitle(tr_("Dock.SettingsTitle"));
    auto* dlgRoot = new QVBoxLayout(m_settings);

    auto* storeBox = new QGroupBox(tr_("Dock.Storage"), m_settings);
    auto* form = new QFormLayout(storeBox);
    m_accountId = new QLineEdit(storeBox);
    m_endpoint  = new QLineEdit(storeBox);
    m_bucket    = new QLineEdit(storeBox);
    m_keyId     = new QLineEdit(storeBox);
    m_secret    = new QLineEdit(storeBox);
    m_secret->setEchoMode(QLineEdit::Password);
    m_region    = new QLineEdit(storeBox);
    m_roomId    = new QLineEdit(storeBox);
    m_prebuffer = new QSpinBox(storeBox);
    m_prebuffer->setRange(0, 10);
    m_prebuffer->setToolTip(tr_("Dock.PrebufferHint"));
    form->addRow(tr_("R2AccountID"), m_accountId);
    form->addRow(tr_("EndpointHost"), m_endpoint);
    form->addRow(tr_("Bucket"), m_bucket);
    form->addRow(tr_("AccessKeyID"), m_keyId);
    form->addRow(tr_("SecretKey"), m_secret);
    form->addRow(tr_("Region"), m_region);
    form->addRow(tr_("RoomID"), m_roomId);
    form->addRow(tr_("Prebuffer"), m_prebuffer);
    dlgRoot->addWidget(storeBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, m_settings);
    dlgRoot->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, m_settings, &QDialog::accept);

    {
        const DecoderSettings cfg = decoder_settings();
        m_accountId->setText(QString::fromStdString(cfg.r2_account_id));
        m_endpoint->setText(QString::fromStdString(cfg.endpoint_host));
        m_bucket->setText(QString::fromStdString(cfg.bucket));
        m_keyId->setText(QString::fromStdString(cfg.access_key_id));
        m_secret->setText(QString::fromStdString(cfg.secret_access_key));
        m_region->setText(QString::fromStdString(cfg.region));
        m_roomId->setText(QString::fromStdString(cfg.room_id));
        m_prebuffer->setValue(cfg.prebuffer_segments);
    }
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId,
                          m_secret, m_region, m_roomId })
        connect(e, &QLineEdit::editingFinished, this,
                &DecoderDock::onSaveSettings);
    connect(m_prebuffer, &QSpinBox::editingFinished, this,
            &DecoderDock::onSaveSettings);

    root->addStretch(1);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &DecoderDock::refresh);
    m_timer->start(500);
    refresh();
}

void DecoderDock::onOpenSettings() {
    if (!m_settings) return;
    m_settings->exec();
    onSaveSettings();
    decoder_reconfigure_all();   // apply straight away
}

void DecoderDock::onSaveSettings() {
    // Trimmed: these are pasted from a dashboard, and a stray space produces
    // failures that look nothing like their cause.
    DecoderSettings cfg = decoder_settings();
    cfg.r2_account_id     = m_accountId->text().trimmed().toStdString();
    cfg.endpoint_host     = m_endpoint->text().trimmed().toStdString();
    cfg.bucket            = m_bucket->text().trimmed().toStdString();
    cfg.access_key_id     = m_keyId->text().trimmed().toStdString();
    cfg.secret_access_key = m_secret->text().trimmed().toStdString();
    cfg.region            = m_region->text().trimmed().toStdString();
    cfg.room_id           = m_roomId->text().trimmed().toStdString();
    cfg.prebuffer_segments = m_prebuffer->value();
    set_decoder_settings(cfg);
}

void DecoderDock::onStart() {
    // Apply any edits before (re)starting, so the button always uses what is
    // on screen, and make existing sources re-read them.
    onSaveSettings();
    decoder_reconfigure_all();
    // Explicit "begin playing / reload": jump to the live edge, which also
    // re-anchors the clock and restarts the decoder. Useful after the encoder
    // has been restarted, or when a source has been sitting idle.
    decoder_jump_live_all();
}

void DecoderDock::onPause()    { decoder_pause_all(); }
void DecoderDock::onResume()   { decoder_resume_all(); }
void DecoderDock::onJumpLive() { decoder_jump_live_all(); }

void DecoderDock::onJumpMarker() {
    const QString id = m_markers->currentData().toString();
    if (id.isEmpty()) return;
    decoder_jump_to_marker(id.toStdString());
}

void DecoderDock::onSeek(unsigned long long seq) {
    decoder_seek(seq);
}

void DecoderDock::refresh() {
    DecoderSnapshot s;
    if (!decoder_snapshot(s)) {
        m_room->setText(tr_("Dock.NoSource"));
        m_state->setText(QString());
        m_behind->setText("—");
        m_error->hide();
        m_pause->setEnabled(false);
        m_resume->setEnabled(false);
        m_live->setEnabled(false);
        m_jumpMarker->setEnabled(false);
        return;
    }

    m_room->setText(QString::fromStdString(s.room_id));
    m_pause->setEnabled(!s.paused);
    m_resume->setEnabled(s.paused);
    m_live->setEnabled(true);
    m_jumpMarker->setEnabled(m_markers->count() > 0);

    // room state, matching RoomState
    switch (s.room_state) {
        case 2:  // Live
            m_state->setText(s.paused ? tr_("Dock.Held") : tr_("Dock.Live"));
            m_state->setStyleSheet(s.paused ? "color: #e0a020; font-weight: bold;"
                                            : "color: #e5484d; font-weight: bold;");
            break;
        case 3:
            m_state->setText(tr_("Dock.Ended"));
            m_state->setStyleSheet("color: palette(mid);");
            break;
        case 1:
            m_state->setText(tr_("Dock.Offline"));
            m_state->setStyleSheet("color: #e5484d;");
            break;
        default:
            m_state->setText(tr_("Dock.Connecting"));
            m_state->setStyleSheet("color: palette(mid);");
            break;
    }

    // Lead with the clock time being shown — the thing an operator can match
    // against what is happening in the room — and express the offset in plain
    // language rather than as a signed number.
    if (s.behind_live_s < 1.0) {
        m_behind->setText(tr_("Dock.ShowingNow"));
        m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #8fd3b4;");
    } else {
        m_behind->setText(tr_("Dock.Showing").arg(clock_time(s.playhead_ms))
                          + "  —  "
                          + tr_("Dock.BehindBy")
                              .arg(friendly_duration(s.behind_live_s)));
        m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #e0a020;");
    }

    m_timeline->setRange(s.first_available, s.live_edge);
    m_timeline->setHead(s.head);
    // buffered_ahead_s is a duration; convert back to a sequence for display.
    const double seg = 6.0;   // display approximation; exact value is per-event
    m_timeline->setBufferedTo(s.head +
        (unsigned long long)(s.buffered_ahead_s / seg));

    std::vector<unsigned long long> mseqs;
    // Rebuild the marker list only when it changes, so the combo doesn't
    // reset while an operator is using it.
    if (s.markers.size() != m_marker_count) {
        m_marker_count = s.markers.size();
        const QString keep = m_markers->currentData().toString();
        m_markers->clear();
        for (const auto& m : s.markers) {
            const QString when = m.at_ms > 0 ? clock_time(m.at_ms) : QString();
            const QString label = when.isEmpty()
                ? QString::fromStdString(m.label)
                : when + "   " + QString::fromStdString(m.label);
            m_markers->addItem(label, QString::fromStdString(m.id));
        }
        const int idx = m_markers->findData(keep);
        if (idx >= 0) m_markers->setCurrentIndex(idx);
    }

    m_buffered->setText(friendly_duration(s.buffered_ahead_s));
    // A count of cached segments means nothing to an operator; the useful
    // figure is how far back they could rewind.
    {
        const double back = (s.playhead_ms > s.earliest_ms && s.earliest_ms > 0)
            ? (double)(s.playhead_ms - s.earliest_ms) / 1000.0 : 0.0;
        m_cached->setText(friendly_duration(back));
    }
    m_marker->setText(s.current_marker.empty()
                        ? QString("—")
                        : QString::fromStdString(s.current_marker));
    m_audio->setText(s.audio_channels > 0
                        ? tr_("Dock.Channels").arg(s.audio_channels)
                        : QString("—"));

    if (!s.last_error.empty()) {
        m_error->setText(QString::fromStdString(s.last_error));
        m_error->show();
    } else {
        m_error->hide();
    }
}

} // namespace multisite_obs
