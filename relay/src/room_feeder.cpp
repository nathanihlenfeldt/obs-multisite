#include "room_feeder.h"

#include <algorithm>
#include <chrono>

namespace multisite_relay {

using namespace multisite;

namespace {
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

RoomFeeder::RoomFeeder(FeederConfig cfg) : m_cfg(std::move(cfg)) {
    m_tx = std::make_unique<S3Transport>(m_cfg.storage);

    DecoderConfig dc;
    dc.room_id = m_cfg.room_id;
    dc.cache_dir = m_cfg.cache_dir;
    dc.buffer_minutes = m_cfg.buffer_minutes;
    dc.stale_after_ms = m_cfg.stale_after_ms;
    // The relay never scrubs backwards, but it does sit behind live, so what
    // it needs kept is whatever the furthest-behind destination has not
    // reached yet, with margin for a restart.
    dc.keep_behind_segments = 100;
    dc.prebuffer_segments = 0;
    m_session = std::make_unique<DecoderSession>(dc, *m_tx);
}

RoomFeeder::~RoomFeeder() { stop(); }

std::string RoomFeeder::check_storage() { return m_tx->self_test(); }

void RoomFeeder::start() {
    if (m_running.exchange(true)) return;
    m_thread = std::thread([this] { run(); });
}

void RoomFeeder::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}

void RoomFeeder::set_lowest_reader(uint64_t seq) {
    m_lowest_reader = seq;
    m_reader_set = true;
}

void RoomFeeder::run() {
    int64_t next_poll = 0;
    while (m_running) {
        const int64_t t = now_ms();

        if (t >= next_poll) {
            next_poll = t + m_cfg.poll_interval_ms;
            m_session->poll(t);

            // event.json is written once at Go Live and carries what the codec
            // gate needs: the video codec and the segment duration. Fetch it
            // when the event changes, not on every pass.
            const std::string ev = m_session->event_id();
            bool need = false;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                need = !ev.empty() && (ev != m_info_event_id || !m_have_info);
            }
            if (need) {
                auto r = m_tx->get(event_prefix_for(ev) + "event.json");
                if (r.success) {
                    try {
                        EventInfo info = EventInfo::from_json(
                            std::string(r.body.begin(), r.body.end()));
                        std::lock_guard<std::mutex> lk(m_mtx);
                        m_info = std::move(info);
                        m_info_event_id = ev;
                        m_have_info = true;
                    } catch (...) {
                        // Left unset: a snapshot without it reports that it
                        // does not know the video format, and plan_stream
                        // refuses rather than guessing.
                    }
                }
            }

            // Park the download window where the destinations are reading,
            // not at the live edge. Without this the relay would be fetching
            // segments three minutes newer than the ones it needs, and the
            // ones it does need would age out of the cache underneath it.
            if (m_reader_set) {
                const uint64_t want = m_lowest_reader.load();
                if (want >= m_session->earliest_available() &&
                    m_session->playback_head() != want)
                    m_session->seek(want);
            }
        }

        // Downloads run flat out until the buffer target is met, which is what
        // banks content ahead of a dropout rather than trickling along at
        // playback speed.
        const int got = m_session->pump_downloads(4);
        if (got == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

RoomSnapshot RoomFeeder::snapshot() const {
    RoomSnapshot s;
    s.room = m_session->room_state();
    s.event_id = m_session->event_id();
    s.latest_seq = m_session->live_edge();
    s.first_available_seq = m_session->earliest_available();
    s.last_error = m_session->last_error();

    const auto& st = m_session->stats();
    s.downloaded = st.downloaded;
    s.checksum_failures = st.checksum_failures;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_have_info && m_info_event_id == s.event_id) {
            s.have_event_info = true;
            s.manifest.video = m_info.video;
            if (m_info.segment_duration_s > 0.1)
                s.segment_duration_s = m_info.segment_duration_s;
            // Only as a fallback: the live manifest is what a decoder trusts
            // for the audio layout, so it wins when it has anything to say.
            s.manifest.audio_tracks = m_info.audio_tracks;
        }
    }

    auto live_tracks = m_session->audio_layout();
    if (!live_tracks.empty()) s.manifest.audio_tracks = std::move(live_tracks);

    s.manifest.latest_seq = s.latest_seq;
    s.manifest.first_available_seq = s.first_available_seq;
    s.manifest.event_id = s.event_id;
    return s;
}

bool RoomFeeder::has_segment(uint64_t seq) const {
    return m_session->cache().has(seq);
}

std::optional<std::vector<uint8_t>> RoomFeeder::load_segment(uint64_t seq) const {
    return m_session->cache().load(seq);
}

std::optional<std::vector<uint8_t>> RoomFeeder::load_init() const {
    return m_session->cache().load_init();
}

} // namespace multisite_relay
