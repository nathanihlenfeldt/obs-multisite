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
#include <QStringList>
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
    setMouseTracking(true);          // needed for the hover readout
    setToolTip(tr_("Dock.TimelineHint"));
}

QSize TimelineBar::minimumSizeHint() const { return QSize(160, 46); }

void TimelineBar::setSpan(long long earliest_ms, long long live_ms) {
    if (m_earliest == earliest_ms && m_live == live_ms) return;
    m_earliest = earliest_ms; m_live = live_ms;
    update();
}
void TimelineBar::setPlayhead(long long ms) {
    if (m_head == ms) return;
    m_head = ms; update();
}
void TimelineBar::setDownloaded(std::vector<std::pair<long long, long long>> spans) {
    if (m_downloaded == spans) return;
    m_downloaded = std::move(spans); update();
}
void TimelineBar::setMarkers(std::vector<long long> times_ms) {
    if (m_markers == times_ms) return;
    m_markers = std::move(times_ms); update();
}

double TimelineBar::fraction(long long ms) const {
    if (m_live <= m_earliest) return 0.0;
    if (ms <= m_earliest) return 0.0;
    if (ms >= m_live) return 1.0;
    return (double)(ms - m_earliest) / (double)(m_live - m_earliest);
}

long long TimelineBar::timeAt(int x) const {
    if (m_live <= m_earliest || width() <= 0) return 0;
    double f = (double)x / (double)width();
    f = std::min(1.0, std::max(0.0, f));
    return m_earliest + (long long)(f * (double)(m_live - m_earliest));
}

void TimelineBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int h = 10;
    const int y = 16;                 // room for the clock scale above
    const int w = width();

    // The recording that still exists in storage.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x36, 0x3b, 0x41));
    p.drawRoundedRect(QRect(0, y, w, h), 4, 4);

    if (m_live <= m_earliest) return;

    // Three states, three distinct colours — and no translucent overlays.
    // Layering a see-through "played" band over the downloaded band produced
    // a second greenish shade that meant nothing, which is exactly the kind of
    // thing an operator should never have to decode mid-service.
    //
    //   grey  = exists in storage, not downloaded here
    //   blue  = downloaded and already played
    //   green = downloaded and ready to play  ← the safety buffer
    const int headX = (int)(fraction(m_head) * w);
    for (const auto& sp : m_downloaded) {
        const int x1 = (int)(fraction(sp.first) * w);
        const int x2 = (int)(fraction(sp.second) * w);
        if (x2 < x1) continue;
        // Split each downloaded span at the playhead.
        const int mid = std::min(std::max(headX, x1), x2);
        if (mid > x1) {
            p.setBrush(QColor(0x35, 0x5a, 0x7a));          // played
            p.drawRect(QRect(x1, y, mid - x1, h));
        }
        if (x2 > mid) {
            p.setBrush(QColor(0x35, 0xc4, 0x89));          // ready to play
            p.drawRect(QRect(mid, y, std::max(1, x2 - mid), h));
        }
    }

    // Clock scale: a few labelled ticks so positions mean something.
    p.setPen(QPen(QColor(0x7f, 0x86, 0x8e)));
    QFont f = p.font(); f.setPointSizeF(f.pointSizeF() - 1.5); p.setFont(f);
    const int ticks = std::max(2, std::min(5, w / 90));
    for (int i = 0; i <= ticks; ++i) {
        const double frac = (double)i / ticks;
        const long long t = m_earliest +
            (long long)(frac * (double)(m_live - m_earliest));
        const int x = (int)(frac * w);
        p.drawLine(x, y - 4, x, y - 1);
        const QString label =
            QDateTime::fromMSecsSinceEpoch((qint64)t).toString("HH:mm");
        QRect r(x - 22, 0, 44, 12);
        p.drawText(r, (i == 0 ? Qt::AlignLeft : (i == ticks ? Qt::AlignRight
                                                            : Qt::AlignHCenter))
                      | Qt::AlignVCenter, label);
    }

    // Marker ticks.
    p.setPen(QPen(QColor(0xe0, 0xa0, 0x20), 2));
    for (long long t : m_markers) {
        const int x = (int)(fraction(t) * w);
        p.drawLine(x, y - 2, x, y + h + 2);
    }

    // Live edge.
    p.setPen(QPen(QColor(0xe5, 0x48, 0x4d), 2));
    p.drawLine(w - 1, y - 3, w - 1, y + h + 3);

    // Playhead.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xdf, 0xe3, 0xe7));
    p.drawEllipse(QPoint(headX, y + h / 2), 6, 6);
    p.setPen(QPen(QColor(0x3b, 0x82, 0xc4), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPoint(headX, y + h / 2), 6, 6);

    // Hover: a following marker and the recorded time under the cursor, so the
    // operator knows what they are jogging to BEFORE they click.
    if (m_hoverX >= 0) {
        const long long t = timeAt(m_hoverX);
        p.setPen(QPen(QColor(0xdf, 0xe3, 0xe7, 160), 1, Qt::DashLine));
        p.drawLine(m_hoverX, y - 6, m_hoverX, y + h + 6);
        const QString label =
            QDateTime::fromMSecsSinceEpoch((qint64)t).toString("HH:mm:ss");
        p.setPen(QPen(QColor(0xff, 0xff, 0xff)));
        QRect box(m_hoverX - 30, y + h + 4, 60, 14);
        if (box.left() < 0) box.moveLeft(0);
        if (box.right() > w) box.moveRight(w);
        p.setBrush(QColor(0x1a, 0x1d, 0x20, 210));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(box, 3, 3);
        p.setPen(QPen(QColor(0xff, 0xff, 0xff)));
        p.drawText(box, Qt::AlignCenter, label);
    }
}

void TimelineBar::mousePressEvent(QMouseEvent* e) {
    const long long t = timeAt(e->pos().x());
    if (t > 0) emit seekRequested(t);
}

void TimelineBar::mouseMoveEvent(QMouseEvent* e) {
    m_hoverX = e->pos().x();
    update();
}

void TimelineBar::leaveEvent(QEvent*) {
    m_hoverX = -1;
    update();
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

    // Legend: three colours, stated once, so nobody has to guess what the
    // bar is telling them.
    {
        auto* leg = new QHBoxLayout();
        leg->setSpacing(10);
        struct { const char* key; const char* colour; } items[] = {
            { "Dock.LegendReady",  "#35c489" },
            { "Dock.LegendPlayed", "#355a7a" },
            { "Dock.LegendStored", "#363b41" },
        };
        for (auto& it : items) {
            auto* sw = new QLabel(this);
            sw->setFixedSize(10, 10);
            sw->setStyleSheet(QString("background:%1; border-radius:2px;")
                                .arg(it.colour));
            auto* tx = new QLabel(tr_(it.key), this);
            tx->setStyleSheet("color: palette(text); opacity: 0.75;");
            QFont f = tx->font(); f.setPointSizeF(f.pointSizeF() - 1.0);
            tx->setFont(f);
            leg->addWidget(sw);
            leg->addWidget(tx);
        }
        leg->addStretch(1);
        root->addLayout(leg);
    }

    // Load, then play. Loading fills the buffer; Play puts it to air. Keeping
    // these separate is how an operator prepares before a service rather than
    // having playback start the moment enough has arrived.
    auto* startRow = new QHBoxLayout();
    m_start = new QPushButton(tr_("Dock.Load"), this);
    m_start->setToolTip(tr_("Dock.LoadHint"));
    m_play  = new QPushButton(tr_("Dock.Play"), this);
    m_stop  = new QPushButton(tr_("Dock.Stop"), this);
    startRow->addWidget(m_start);
    startRow->addWidget(m_play);
    startRow->addWidget(m_stop);
    root->addLayout(startRow);
    connect(m_start, &QPushButton::clicked, this, &DecoderDock::onStart);
    connect(m_play,  &QPushButton::clicked, this, &DecoderDock::onPlay);
    connect(m_stop,  &QPushButton::clicked, this, &DecoderDock::onStop);

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

    // Jog: coarse and honest. Seeking lands within about a second, so
    // frame-level steps would be misleading.
    auto* jogRow = new QHBoxLayout();
    struct { const char* text; double secs; } jogs[] = {
        { "-1 min", -60.0 }, { "-10 s", -10.0 }, { "-1 s", -1.0 },
        { "+1 s", 1.0 }, { "+10 s", 10.0 }, { "+1 min", 60.0 },
    };
    for (auto& j : jogs) {
        auto* b = new QPushButton(QString::fromUtf8(j.text), this);
        b->setMaximumWidth(64);
        const double secs = j.secs;
        connect(b, &QPushButton::clicked, this, [this, secs] { onJog(secs); });
        jogRow->addWidget(b);
    }
    root->addLayout(jogRow);

    // Sit at a fixed delay behind the main site — the usual way a campus runs
    // when it wants a safety margin.
    auto* delayRow = new QHBoxLayout();
    delayRow->addWidget(new QLabel(tr_("Dock.DelayFromLive"), this));
    m_delayMins = new QSpinBox(this);
    m_delayMins->setRange(0, 120);
    m_delayMins->setSuffix(tr_("Dock.Minutes"));
    m_delayMins->setValue(0);
    m_goTo = new QPushButton(tr_("Dock.GoTo"), this);
    delayRow->addWidget(m_delayMins);
    delayRow->addWidget(m_goTo);
    delayRow->addStretch(1);
    m_lock = new QPushButton(tr_("Dock.Lock"), this);
    m_lock->setCheckable(true);
    m_lock->setToolTip(tr_("Dock.LockHint"));
    delayRow->addWidget(m_lock);
    root->addLayout(delayRow);
    connect(m_goTo, &QPushButton::clicked, this, &DecoderDock::onGoToDelay);
    connect(m_lock, &QPushButton::toggled, this, &DecoderDock::onLockToggled);

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
    m_bufferMins = new QSpinBox(storeBox);
    m_bufferMins->setRange(1, 60);
    m_bufferMins->setSuffix(tr_("Dock.Minutes"));
    m_bufferMins->setToolTip(tr_("BufferMinutesHint"));
    form->addRow(tr_("BufferMinutes"), m_bufferMins);
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
        m_bufferMins->setValue(cfg.buffer_minutes);
    }
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId,
                          m_secret, m_region, m_roomId })
        connect(e, &QLineEdit::editingFinished, this,
                &DecoderDock::onSaveSettings);
    for (QSpinBox* sb : { m_prebuffer, m_bufferMins })
        connect(sb, &QSpinBox::editingFinished, this,
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
    cfg.buffer_minutes     = m_bufferMins->value();
    set_decoder_settings(cfg);
}

void DecoderDock::onStart() {
    // Load: apply settings, (re)connect and start filling the buffer. It does
    // NOT go to air — that is what Play is for.
    onSaveSettings();
    decoder_reconfigure_all();
    decoder_jump_live_all();
}

void DecoderDock::onPlay()  { decoder_play_all(); }
void DecoderDock::onStop()  { decoder_stop_all(); }
void DecoderDock::onJog(double seconds) { decoder_jog(seconds); }
void DecoderDock::onGoToDelay() {
    decoder_set_delay(m_delayMins ? m_delayMins->value() * 60.0 : 0.0);
}
void DecoderDock::onLockToggled(bool on) {
    decoder_set_locked(on);
    if (m_lock) m_lock->setText(on ? tr_("Dock.Unlock") : tr_("Dock.Lock"));
}

void DecoderDock::onPause()    { decoder_pause_all(); }
void DecoderDock::onResume()   { decoder_resume_all(); }
void DecoderDock::onJumpLive() { decoder_jump_live_all(); }

void DecoderDock::onJumpMarker() {
    const QString id = m_markers->currentData().toString();
    if (id.isEmpty()) return;
    decoder_jump_to_marker(id.toStdString());
}

void DecoderDock::onSeek(long long wall_ms) {
    decoder_seek_time(wall_ms);
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
    // Lock disables everything that changes what is on air.
    const bool ctl = !s.locked;
    m_pause->setEnabled(ctl && s.playing && !s.paused);
    m_resume->setEnabled(ctl && s.paused);
    m_live->setEnabled(ctl);
    m_play->setEnabled(ctl && !s.playing);
    m_stop->setEnabled(ctl && s.playing);
    m_start->setEnabled(ctl);
    m_goTo->setEnabled(ctl);
    m_jumpMarker->setEnabled(ctl && m_markers->count() > 0);
    if (m_lock && m_lock->isChecked() != s.locked) {
        m_lock->blockSignals(true);
        m_lock->setChecked(s.locked);
        m_lock->setText(s.locked ? tr_("Dock.Unlock") : tr_("Dock.Lock"));
        m_lock->blockSignals(false);
    }

    // room state, matching RoomState
    switch (s.room_state) {
        case 2:  // Live
            m_state->setText(s.paused ? tr_("Dock.Held") : tr_("Dock.Live"));
            m_state->setStyleSheet(s.paused ? "color: #e0a020; font-weight: bold;"
                                            : "color: #e5484d; font-weight: bold;");
            break;
        case 3:
            // Two different situations, and an operator needs to tell them
            // apart: the service they were watching has just finished, versus
            // this was already a recording when they loaded it.
            if (s.was_live) {
                m_state->setText(tr_("Dock.BroadcastEnded"));
                m_state->setStyleSheet("color: #e0a020; font-weight: bold;");
            } else {
                m_state->setText(tr_("Dock.NotLive"));
                m_state->setStyleSheet("color: #8fd3b4;");
            }
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
    if (s.ended) {
        // A finished recording: show where you are in it and how much is left.
        // "Behind live" is meaningless once there is no live edge to be behind.
        if (s.at_end) {
            m_behind->setText(tr_("Dock.AtEnd"));
            m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #8b9198;");
        } else {
            const double left = (s.end_ms > s.playhead_ms)
                ? (double)(s.end_ms - s.playhead_ms) / 1000.0 : 0.0;
            m_behind->setText(tr_("Dock.Showing").arg(clock_time(s.playhead_ms))
                              + "  —  "
                              + tr_("Dock.Remaining")
                                  .arg(friendly_duration(left)));
            m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #8fd3b4;");
        }
    } else if (s.head > s.live_edge && s.live_edge > 0) {
        // Caught right up: nothing new has been published yet. This is normal
        // and must not look like a fault.
        m_behind->setText(tr_("Dock.WaitingForMain"));
        m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #8fd3b4;");
    } else if (s.behind_live_s < 1.0) {
        m_behind->setText(tr_("Dock.ShowingNow"));
        m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #8fd3b4;");
    } else {
        m_behind->setText(tr_("Dock.Showing").arg(clock_time(s.playhead_ms))
                          + "  —  "
                          + tr_("Dock.BehindBy")
                              .arg(friendly_duration(s.behind_live_s)));
        m_behind->setStyleSheet("font-size: 18px; font-weight: 500; color: #e0a020;");
    }

    // Timeline entirely in clock time now, with the real downloaded ranges.
    // For a finished recording the span runs to its true end, not to the last
    // segment's start time.
    m_timeline->setSpan(s.earliest_ms,
                        (s.ended && s.end_ms > 0) ? s.end_ms : s.live_ms);
    m_timeline->setPlayhead(s.playhead_ms);
    m_timeline->setDownloaded(s.cached_spans);
    {
        std::vector<long long> mt;
        for (const auto& m : s.markers) if (m.at_ms > 0) mt.push_back(m.at_ms);
        m_timeline->setMarkers(std::move(mt));
    }

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

    // The reliability figure: how long this campus could keep broadcasting if
    // the connection died right now.
    m_buffered->setText(friendly_duration(s.buffered_ahead_s));
    m_buffered->setToolTip(tr_("Dock.BufferedHint"));
    // How far back the recording still exists in storage (not on this PC).
    {
        const double back = (s.playhead_ms > s.earliest_ms && s.earliest_ms > 0)
            ? (double)(s.playhead_ms - s.earliest_ms) / 1000.0 : 0.0;
        m_cached->setText(friendly_duration(back));
        m_cached->setToolTip(tr_("Dock.RewindHint"));
    }
    m_marker->setText(s.current_marker.empty()
                        ? QString("—")
                        : QString::fromStdString(s.current_marker));
    // Show what the audio actually contains, using the names the main site
    // published — otherwise those names are write-only and the channel count
    // means nothing to the person watching.
    if (s.audio_channels <= 0) {
        m_audio->setText(QString("—"));
        m_audio->setToolTip(QString());
    } else if (!s.channel_labels.empty()) {
        QStringList names;
        for (const auto& c : s.channel_labels)
            names << QString::fromStdString(c);
        m_audio->setText(tr_("Dock.Channels").arg(s.audio_channels));
        m_audio->setToolTip(names.join(", "));
        // First few inline so it is visible without hovering.
        const int show = qMin(3, names.size());
        m_audio->setText(tr_("Dock.Channels").arg(s.audio_channels) + "  (" +
                         QStringList(names.mid(0, show)).join(", ") +
                         (names.size() > show ? "…" : "") + ")");
    } else {
        m_audio->setText(tr_("Dock.Channels").arg(s.audio_channels) +
                         (s.audio_track_label.empty()
                            ? QString()
                            : "  " + QString::fromStdString(s.audio_track_label)));
        m_audio->setToolTip(QString());
    }

    if (!s.last_error.empty()) {
        m_error->setText(QString::fromStdString(s.last_error));
        m_error->show();
    } else {
        m_error->hide();
    }
}

} // namespace multisite_obs
