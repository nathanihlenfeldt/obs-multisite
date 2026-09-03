#include "decoder_dock.h"

#include "../multisite_ui.h"
#include "../plugin_log.h"

#include <obs-module.h>

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
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
        cap->setStyleSheet("color: palette(mid);");
        out = new QLabel("—", box);
        grid->addWidget(cap, r, c * 2);
        grid->addWidget(out, r, c * 2 + 1);
    };
    addStat(0, 0, "Dock.Buffered", m_buffered);
    addStat(0, 1, "Dock.Cached",   m_cached);
    addStat(1, 0, "Dock.Marker",   m_marker);
    addStat(1, 1, "Dock.Audio",    m_audio);
    m_error = new QLabel(QString(), box);
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color: #e5484d;");
    m_error->hide();
    grid->addWidget(m_error, 2, 0, 1, 4);
    root->addWidget(box);

    root->addStretch(1);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &DecoderDock::refresh);
    m_timer->start(500);
    refresh();
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

    if (s.behind_live_s < 1.0) {
        m_behind->setText(tr_("Dock.AtLive"));
        m_behind->setStyleSheet("font-size: 20px; font-weight: 500; color: #8fd3b4;");
    } else {
        const int mins = (int)(s.behind_live_s / 60.0);
        const int secs = (int)s.behind_live_s % 60;
        m_behind->setText(
            (mins > 0 ? QString("-%1:%2").arg(mins).arg(secs, 2, 10, QChar('0'))
                      : QString("-%1s").arg(secs)) + " " + tr_("Dock.BehindLive"));
        m_behind->setStyleSheet("font-size: 20px; font-weight: 500; color: #e0a020;");
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
        for (const auto& m : s.markers)
            m_markers->addItem(QString::fromStdString(m.first),
                               QString::fromStdString(m.second));
        const int idx = m_markers->findData(keep);
        if (idx >= 0) m_markers->setCurrentIndex(idx);
    }

    m_buffered->setText(QString::number(s.buffered_ahead_s, 'f', 0) + " s");
    m_cached->setText(QString::number((qulonglong)s.cached));
    m_marker->setText(s.current_marker.empty()
                        ? QString("—")
                        : QString::fromStdString(s.current_marker));
    m_audio->setText(s.audio_channels > 0
                        ? QString::number(s.audio_channels) + " ch"
                        : QString("—"));

    if (!s.last_error.empty()) {
        m_error->setText(QString::fromStdString(s.last_error));
        m_error->show();
    } else {
        m_error->hide();
    }
}

} // namespace multisite_obs
