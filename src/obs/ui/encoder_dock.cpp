#include "encoder_dock.h"

#include "../broadcast_controller.h"
#include "../multisite_ui.h"
#include "../plugin_log.h"

#include <obs-module.h>

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace multisite_obs {

static QString tr_(const char* key) {
    return QString::fromUtf8(obs_module_text(key));
}

EncoderDock::EncoderDock(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // ── Status first: what an operator looks at mid-service ─────────────────
    auto* statusBox = new QGroupBox(tr_("Dock.Status"), this);
    auto* grid = new QGridLayout(statusBox);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);

    auto addStat = [&](int row, int col, const char* labelKey, QLabel*& out) {
        auto* cap = new QLabel(tr_(labelKey), statusBox);
        cap->setStyleSheet("color: palette(mid);");
        out = new QLabel("—", statusBox);
        grid->addWidget(cap, row, col * 2);
        grid->addWidget(out, row, col * 2 + 1);
    };

    m_state = new QLabel(tr_("Dock.Idle"), statusBox);
    m_state->setStyleSheet("font-weight: bold;");
    grid->addWidget(m_state, 0, 0, 1, 2);
    addStat(0, 1, "Dock.Uptime",    m_uptime);
    addStat(1, 0, "Dock.Confirmed", m_confirmed);
    addStat(1, 1, "Dock.Queue",     m_queue);
    addStat(2, 0, "Dock.Retries",   m_retries);
    addStat(2, 1, "Dock.Uploaded",  m_data);
    addStat(3, 0, "Dock.Link",      m_link);

    m_error = new QLabel(QString(), statusBox);
    m_error->setWordWrap(true);
    m_error->setStyleSheet("color: #e5484d;");
    m_error->hide();
    grid->addWidget(m_error, 4, 0, 1, 4);
    root->addWidget(statusBox);

    // ── Go live / end ────────────────────────────────────────────────────────
    auto* row = new QHBoxLayout();
    m_goLive = new QPushButton(tr_("Dock.GoLive"), this);
    m_end    = new QPushButton(tr_("Dock.End"), this);
    m_end->setEnabled(false);
    row->addWidget(m_goLive);
    row->addWidget(m_end);
    root->addLayout(row);

    // ── Markers ──────────────────────────────────────────────────────────────
    auto* markerBox = new QGroupBox(tr_("Dock.Markers"), this);
    auto* mrow = new QGridLayout(markerBox);
    for (int i = 0; i < 4; ++i) {
        m_markers[i] = new QPushButton(QString("—"), markerBox);
        m_markers[i]->setEnabled(false);
        mrow->addWidget(m_markers[i], i / 2, i % 2);
        connect(m_markers[i], &QPushButton::clicked, this,
                [this, i] { onMarker(i); });
    }
    root->addWidget(markerBox);

    // ── Storage ──────────────────────────────────────────────────────────────
    auto* storeBox = new QGroupBox(tr_("Dock.Storage"), this);
    auto* form = new QFormLayout(storeBox);
    m_accountId = new QLineEdit(storeBox);
    m_endpoint  = new QLineEdit(storeBox);
    m_bucket    = new QLineEdit(storeBox);
    m_keyId     = new QLineEdit(storeBox);
    m_secret    = new QLineEdit(storeBox);
    m_secret->setEchoMode(QLineEdit::Password);
    m_region    = new QLineEdit(storeBox);
    m_room      = new QLineEdit(storeBox);
    m_tags      = new QCheckBox(tr_("UseObjectTags"), storeBox);
    form->addRow(tr_("R2AccountID"), m_accountId);
    form->addRow(tr_("EndpointHost"), m_endpoint);
    form->addRow(tr_("Bucket"), m_bucket);
    form->addRow(tr_("AccessKeyID"), m_keyId);
    form->addRow(tr_("SecretKey"), m_secret);
    form->addRow(tr_("Region"), m_region);
    form->addRow(tr_("RoomID"), m_room);
    form->addRow(QString(), m_tags);
    root->addWidget(storeBox);

    // ── Media ────────────────────────────────────────────────────────────────
    auto* mediaBox = new QGroupBox(tr_("Dock.Media"), this);
    auto* mform = new QFormLayout(mediaBox);
    m_segDur = new QDoubleSpinBox(mediaBox);
    m_segDur->setRange(2.0, 15.0);
    m_segDur->setSingleStep(0.5);
    m_segDur->setSuffix(" s");
    m_vBitrate = new QSpinBox(mediaBox);
    m_vBitrate->setRange(500, 50000);
    m_vBitrate->setSingleStep(500);
    m_vBitrate->setSuffix(" kbps");
    m_aBitrate = new QSpinBox(mediaBox);
    m_aBitrate->setRange(64, 512);
    m_aBitrate->setSingleStep(32);
    m_aBitrate->setSuffix(" kbps");
    m_tracks = new QSpinBox(mediaBox);
    m_tracks->setRange(1, 6);
    m_trackLabels   = new QLineEdit(mediaBox);
    m_channelLabels = new QLineEdit(mediaBox);
    m_markerLabels  = new QLineEdit(mediaBox);
    mform->addRow(tr_("SegmentDuration"), m_segDur);
    mform->addRow(tr_("Dock.VideoBitrate"), m_vBitrate);
    mform->addRow(tr_("Dock.AudioBitrate"), m_aBitrate);
    mform->addRow(tr_("Dock.AudioTracks"), m_tracks);
    mform->addRow(tr_("TrackLabels"), m_trackLabels);
    mform->addRow(tr_("ChannelLabels"), m_channelLabels);
    mform->addRow(tr_("MarkerLabels"), m_markerLabels);
    root->addWidget(mediaBox);

    root->addStretch(1);

    connect(m_goLive, &QPushButton::clicked, this, &EncoderDock::onGoLive);
    connect(m_end,    &QPushButton::clicked, this, &EncoderDock::onEnd);

    // Settings are saved as they are edited, so nothing is lost if OBS closes
    // unexpectedly — an operator should never have to retype credentials.
    for (QLineEdit* e : { m_accountId, m_endpoint, m_bucket, m_keyId, m_secret,
                          m_region, m_room, m_trackLabels, m_channelLabels,
                          m_markerLabels })
        connect(e, &QLineEdit::editingFinished, this,
                &EncoderDock::onSaveSettings);
    connect(m_tags, &QCheckBox::toggled, this, &EncoderDock::onSaveSettings);
    connect(m_segDur, &QDoubleSpinBox::editingFinished, this,
            &EncoderDock::onSaveSettings);
    for (QSpinBox* sb : { m_vBitrate, m_aBitrate, m_tracks })
        connect(sb, &QSpinBox::editingFinished, this,
                &EncoderDock::onSaveSettings);

    loadIntoFields();

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &EncoderDock::refresh);
    m_timer->start(1000);
    refresh();
}

void EncoderDock::loadIntoFields() {
    auto cfg = BroadcastController::instance().settings();
    cfg.load();
    BroadcastController::instance().set_settings(cfg);

    m_accountId->setText(QString::fromStdString(cfg.r2_account_id));
    m_endpoint->setText(QString::fromStdString(cfg.endpoint_host));
    m_bucket->setText(QString::fromStdString(cfg.bucket));
    m_keyId->setText(QString::fromStdString(cfg.access_key_id));
    m_secret->setText(QString::fromStdString(cfg.secret_access_key));
    m_region->setText(QString::fromStdString(cfg.region));
    m_room->setText(QString::fromStdString(cfg.room_id));
    m_tags->setChecked(cfg.use_object_tags);
    m_segDur->setValue(cfg.segment_duration_s);
    m_vBitrate->setValue(cfg.video_bitrate_kbps);
    m_aBitrate->setValue(cfg.audio_bitrate_kbps);
    m_tracks->setValue(cfg.audio_tracks);
    m_trackLabels->setText(QString::fromStdString(cfg.track_labels));
    m_channelLabels->setText(QString::fromStdString(cfg.channel_labels));
    m_markerLabels->setText(QString::fromStdString(cfg.marker_labels));
}

void EncoderDock::onSaveSettings() {
    BroadcastSettings cfg;
    cfg.r2_account_id     = m_accountId->text().toStdString();
    cfg.endpoint_host     = m_endpoint->text().toStdString();
    cfg.bucket            = m_bucket->text().toStdString();
    cfg.access_key_id     = m_keyId->text().toStdString();
    cfg.secret_access_key = m_secret->text().toStdString();
    cfg.region            = m_region->text().toStdString();
    cfg.room_id           = m_room->text().toStdString();
    cfg.use_object_tags   = m_tags->isChecked();
    cfg.segment_duration_s = m_segDur->value();
    cfg.video_bitrate_kbps = m_vBitrate->value();
    cfg.audio_bitrate_kbps = m_aBitrate->value();
    cfg.audio_tracks       = m_tracks->value();
    cfg.track_labels       = m_trackLabels->text().toStdString();
    cfg.channel_labels     = m_channelLabels->text().toStdString();
    cfg.marker_labels      = m_markerLabels->text().toStdString();
    BroadcastController::instance().set_settings(cfg);
}

void EncoderDock::onGoLive() {
    onSaveSettings();
    std::string err;
    if (!BroadcastController::instance().go_live(err)) {
        // Show the reason here rather than making the operator find the log.
        QMessageBox::warning(this, tr_("Dock.GoLiveFailed"),
                             QString::fromStdString(err));
        return;
    }
    setLiveState(true);
}

void EncoderDock::onEnd() {
    BroadcastController::instance().end_broadcast();
    setLiveState(false);
}

void EncoderDock::onMarker(int index) {
    auto labels = m_markerLabels->text().split(',', Qt::SkipEmptyParts);
    QString label = (index < labels.size())
                      ? labels[index].trimmed()
                      : QString("Marker %1").arg(index + 1);
    BroadcastController::instance().drop_marker(label.toStdString());
}

void EncoderDock::setLiveState(bool live) {
    m_goLive->setEnabled(!live);
    m_end->setEnabled(live);
    for (auto* b : m_markers) b->setEnabled(live);
    // Storage cannot change mid-broadcast.
    for (QWidget* w : { (QWidget*)m_accountId, (QWidget*)m_endpoint,
                        (QWidget*)m_bucket, (QWidget*)m_keyId,
                        (QWidget*)m_secret, (QWidget*)m_region,
                        (QWidget*)m_room, (QWidget*)m_segDur,
                        (QWidget*)m_tracks })
        w->setEnabled(!live);
}

void EncoderDock::refresh() {
    // Marker button captions follow the labels the operator has typed.
    auto labels = m_markerLabels->text().split(',', Qt::SkipEmptyParts);
    for (int i = 0; i < 4; ++i) {
        QString cap = (i < labels.size()) ? labels[i].trimmed()
                                          : QString("Marker %1").arg(i + 1);
        if (m_markers[i]->text() != cap) m_markers[i]->setText(cap);
    }

    auto st = BroadcastController::instance().status();
    setLiveState(st.live);

    if (!st.live) {
        m_state->setText(tr_("Dock.Idle"));
        m_state->setStyleSheet("font-weight: bold; color: palette(mid);");
        m_uptime->setText("—");
        m_error->hide();
        return;
    }

    m_state->setText(tr_("Dock.Broadcasting"));
    m_state->setStyleSheet("font-weight: bold; color: #35c489;");

    const int mins = (int)(st.uptime_s / 60.0);
    const int secs = (int)st.uptime_s % 60;
    m_uptime->setText(QString("%1:%2").arg(mins).arg(secs, 2, 10, QChar('0')));
    m_confirmed->setText(QString::number(st.confirmed));
    m_queue->setText(QString::number((qulonglong)st.pending));
    m_retries->setText(QString::number(st.retries));
    m_data->setText(QString::number(st.bytes / (1024.0 * 1024.0), 'f', 1) + " MB");

    // The reliability signal an operator actually needs mid-service.
    switch (st.link_health) {
        case 0:
            m_link->setText(tr_("Dock.LinkHealthy"));
            m_link->setStyleSheet("color: #35c489;");
            break;
        case 1:
            m_link->setText(tr_("Dock.LinkDegraded"));
            m_link->setStyleSheet("color: #e0a020;");
            break;
        default:
            m_link->setText(tr_("Dock.LinkOffline"));
            m_link->setStyleSheet("color: #e5484d;");
            break;
    }

    if (!st.last_error.empty()) {
        m_error->setText(QString::fromStdString(st.last_error));
        m_error->show();
    } else {
        m_error->hide();
    }
}

} // namespace multisite_obs
