#include "decoder_session.h"
#include "session.h"     // for now_ms()

#include <algorithm>
#include <cstdio>

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

DecoderSession::DecoderSession(DecoderConfig cfg, Transport& transport)
    : m_cfg(std::move(cfg)), m_tx(transport) {
    m_cache = std::make_unique<SegmentCache>(m_cfg.cache_dir, "pending");
}

std::string DecoderSession::event_prefix() const {
    return "events/" + m_event_id + "/";
}
std::string DecoderSession::segment_key(uint64_t seq) const {
    return event_prefix() + "segments/" + seq_name(seq) + ".m4s";
}

std::string DecoderSession::checksum_for(uint64_t seq) const {
    for (const auto& s : m_manifest.segments)
        if (s.seq == seq) return s.checksum;
    return "";     // outside the manifest window: verify not possible
}

// ── Discovery ────────────────────────────────────────────────────────────────
RoomState DecoderSession::poll(int64_t now_override) {
    // Network fetches happen WITHOUT the state lock held.
    //
    // This function used to hold m_mtx across three HTTP requests, and
    // pump_downloads held it across every segment download. Every UI query —
    // the dock refreshes several times a second — then had to wait for
    // whatever download was in flight, which made the interface sluggish and
    // made a timeline click appear to lock OBS up entirely. The lock now
    // protects state only, and is never held across I/O.
    const int64_t now = now_override ? now_override : now_ms();

    // 1. Which event is live in this room?
    auto lp = m_tx.get("rooms/" + m_cfg.room_id + "/live.json");
    if (!lp.success) {
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "live.json: HTTP " + std::to_string(lp.http_status) +
                       " " + lp.error; }
        m_room = RoomState::Offline;
        return m_room;
    }

    LivePointer live;
    try {
        live = LivePointer::from_json(
            std::string(lp.body.begin(), lp.body.end()));
    } catch (...) {
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "live.json is not valid JSON"; }
        m_room = RoomState::Offline;
        return m_room;
    }

    if (live.event_id.empty()) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_room = RoomState::Offline;
        return m_room;
    }

    // 2. Note an event change under the lock, then release it before fetching.
    std::string event_id;
    bool need_markers = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (live.event_id != m_event_id) {
            m_event_id = live.event_id;
            m_cache->set_event(m_event_id);
            m_head_set = false;
            m_init_sent = false;
            m_play = PlayState::Stopped;
            m_markers = MarkerList{};
            m_markers_checked_ms = 0;
            m_saw_live = false;     // a new event has not been seen live yet
            ++m_discontinuity;      // new event: new init segment and timeline
        }
        event_id = m_event_id;
        if (now - m_markers_checked_ms > 5000) {
            m_markers_checked_ms = now;
            need_markers = true;
        }
    }
    const std::string prefix = "events/" + event_id + "/";

    // 3. Manifest, fetched without the lock.
    auto mf = m_tx.get(prefix + "manifest.json");
    if (!mf.success) {
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "manifest.json: HTTP " + std::to_string(mf.http_status) +
                       " " + mf.error; }
        m_room = RoomState::Offline;
        return m_room;
    }
    Manifest fetched;
    try {
        fetched = Manifest::from_json(
            std::string(mf.body.begin(), mf.body.end()));
    } catch (...) {
        std::lock_guard<std::mutex> lk(m_mtx);
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "manifest.json is not valid JSON"; }
        m_room = RoomState::Offline;
        return m_room;
    }

    // 4. Markers (small, and only every few seconds), also unlocked.
    MarkerList markers;
    bool have_markers = false;
    if (need_markers) {
        auto mk = m_tx.get(prefix + "markers.json");
        if (mk.success) {
            try {
                markers = MarkerList::from_json(
                    std::string(mk.body.begin(), mk.body.end()));
                have_markers = true;
            } catch (...) {
                // A malformed markers file must not disturb playback.
            }
        }
        // A 404 simply means no markers have been dropped yet.
    }

    // 5. event.json for the start time, if the manifest lacks it (older
    //    encoders). Fetched unlocked, once per event.
    int64_t started_at = fetched.started_at_ms;
    double  seg_hint = 0.0;
    if (started_at <= 0) {
        auto ev = m_tx.get(prefix + "event.json");
        if (ev.success) {
            try {
                EventInfo info = EventInfo::from_json(
                    std::string(ev.body.begin(), ev.body.end()));
                started_at = info.started_at_ms;
                seg_hint   = info.segment_duration_s;
            } catch (...) {}
        }
    }

    // 6. Commit the new state.
    std::lock_guard<std::mutex> lk(m_mtx);
    m_manifest = std::move(fetched);
    if (started_at > 0) m_manifest.started_at_ms = started_at;
    m_started_at_ms = m_manifest.started_at_ms;   // lock-free for the UI
    if (have_markers) m_markers = std::move(markers);

    m_latest_seq          = m_manifest.latest_seq;
    m_first_available_seq = m_manifest.first_available_seq;
    m_manifest_updated_ms = m_manifest.updated_at_ms;
    if (m_manifest.stream_duration_hint() > 0.1)
        m_segment_duration_s = m_manifest.stream_duration_hint();
    else if (seg_hint > 0.1)
        m_segment_duration_s = seg_hint;

    // Stale detection: an encoder that died leaves live.json pointing at an
    // event whose manifest stops advancing. Don't poll a corpse forever.
    const int64_t age = now - m_manifest_updated_ms;
    if (m_manifest.status == "ended") {
        m_room = RoomState::Ended;
    } else if (m_manifest_updated_ms > 0 && age > m_cfg.stale_after_ms) {
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "manifest last updated " + std::to_string(age / 1000) +
                       "s ago — treating the room as offline"; }
        m_room = RoomState::Offline;
    } else {
        m_room = RoomState::Live;
        m_saw_live = true;          // remembered for the rest of this event
        { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error.clear(); }
    }
    return m_room;
}

// ── Downloading ──────────────────────────────────────────────────────────────

int DecoderSession::pump_downloads(int max) {
    // Same discipline as poll(): the lock is held only to decide WHAT to
    // fetch and to record the result. The downloads themselves — potentially
    // several megabytes each — happen with no lock held, so the UI stays
    // responsive while the buffer fills.
    std::string prefix;
    uint64_t from = 0, to = 0;
    std::vector<std::pair<uint64_t, std::string>> wanted;   // seq, checksum
    bool need_init = false;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_event_id.empty()) return 0;
        prefix = "events/" + m_event_id + "/";
        need_init = !m_cache->has_init();

        if (m_head_set.load()) {
            from = m_head.load();
        } else if (m_room.load() == RoomState::Ended) {
            // A finished recording plays from the beginning, so fetch from the
            // beginning. Downloading from the live edge while playback intends
            // to start at segment zero left the first segment missing and
            // start() refused to begin.
            from = m_first_available_seq.load();
        } else {
            const uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
            from = (m_latest_seq.load() > back) ? (m_latest_seq.load() - back)
                                                : m_first_available_seq.load();
        }
        from = std::max(from, m_first_available_seq.load());

        // Buffer target in minutes of programme, converted to segments. When
        // far behind live this is a wide window on purpose: bank as much as
        // the link can manage rather than trickling at playback speed.
        const double seg = m_segment_duration_s.load() > 0.1 ? m_segment_duration_s.load() : 6.0;
        uint64_t want_ahead = (uint64_t)std::max(
            1.0, ((double)std::max(1, m_cfg.buffer_minutes) * 60.0) / seg);
        if (want_ahead > (uint64_t)m_cfg.max_cached_segments)
            want_ahead = (uint64_t)m_cfg.max_cached_segments;
        to = std::min<uint64_t>(m_latest_seq.load(), from + want_ahead);

        // One copy of the index, rather than a lock acquisition per segment
        // across a window that can be hundreds of segments wide.
        const auto have = m_cache->cached_seqs();
        for (uint64_t sq = from; sq <= to && (int)wanted.size() < max; ++sq) {
            if (have.count(sq)) continue;
            std::string sum;
            for (const auto& ms : m_manifest.segments)
                if (ms.seq == sq) { sum = ms.checksum; break; }
            wanted.emplace_back(sq, sum);
        }
    }

    // ── unlocked from here ───────────────────────────────────────────────────
    if (need_init) {
        auto r = m_tx.get(prefix + "init.mp4");
        if (!r.success) {
            std::lock_guard<std::mutex> lk(m_mtx);
            { std::lock_guard<std::mutex> elk(m_err_mtx); m_last_error = "init.mp4: HTTP " + std::to_string(r.http_status) +
                           " " + r.error; }
            return 0;
        }
        if (!m_cache->store_init(r.body)) return 0;
    }

    int fetched = 0;
    uint64_t dl = 0, dlfail = 0, ckfail = 0;
    std::string err;
    for (const auto& w : wanted) {
        char name[16];
        std::snprintf(name, sizeof(name), "%08llu",
                      (unsigned long long)w.first);
        auto r = m_tx.get(prefix + "segments/" + name + ".m4s");
        if (!r.success) {
            // A 404 usually just means "not published yet" — expected at the
            // live edge, so it is not counted as a failure.
            if (r.http_status != 404) {
                ++dlfail;
                err = "segment " + std::to_string(w.first) + ": HTTP " +
                      std::to_string(r.http_status) + " " + r.error;
            }
            continue;
        }
        if (!m_cache->store(w.first, r.body, w.second)) {
            // Checksum mismatch: don't cache it, so the next pass re-fetches
            // rather than feeding corruption to the decoder.
            ++ckfail;
            err = "segment " + std::to_string(w.first) +
                  " failed checksum verification";
            continue;
        }
        ++dl;
        ++fetched;
    }

    // ── commit ───────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stats.downloaded        += dl;
        m_stats.download_failures += dlfail;
        m_stats.checksum_failures += ckfail;
        if (!err.empty()) {
            std::lock_guard<std::mutex> elk(m_err_mtx);
            m_last_error = err;
        }

        // Bound disk use, but keep plenty of scrub-back room.
        if (m_head_set.load() && m_head.load() > (uint64_t)m_cfg.keep_behind_segments)
            m_cache->prune_below(m_head.load() - (uint64_t)m_cfg.keep_behind_segments);
        if ((int)m_cache->count() > m_cfg.max_cached_segments) {
            const uint64_t lowest = m_cache->lowest_seq();
            const size_t excess =
                m_cache->count() - (size_t)m_cfg.max_cached_segments;
            m_cache->prune_below(lowest + (uint64_t)excess);
        }
    }
    return fetched;
}

// ── Playback ─────────────────────────────────────────────────────────────────
bool DecoderSession::start() {
    std::lock_guard<std::mutex> lk(m_mtx);
    const RoomState rs = m_room.load();
    if (rs != RoomState::Live && rs != RoomState::Ended) return false;
    if (!m_cache->has_init()) return false;

    if (!m_head_set.load()) {
        uint64_t want;
        if (rs == RoomState::Ended) {
            // A recording that has already finished is video-on-demand: start
            // at the beginning. Starting near the end (which is what treating
            // it as "live" does) means loading a finished service and landing
            // twelve seconds from the close.
            want = m_first_available_seq.load();
        } else {
            // Live: start `prebuffer_segments` behind the edge for a cushion.
            const uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
            want = (m_latest_seq.load() > back) ? (m_latest_seq.load() - back)
                                                : m_first_available_seq.load();
        }
        want = std::max(want, m_first_available_seq.load());
        if (!m_cache->has(want)) return false;      // prebuffer not ready yet
        m_head = want;
        m_head_set = true;
    }
    m_play = PlayState::Playing;
    return true;
}

void DecoderSession::pause() {
    std::lock_guard<std::mutex> lk(m_mtx);
    // The head stays exactly where it is; pump_downloads keeps filling ahead,
    // so resuming loses nothing. Pausing from Stopped is also honoured: the
    // operator's intent is "hold", and it must not silently no-op just
    // because playback had not begun yet.
    m_play = PlayState::Paused;
}

void DecoderSession::resume() {
    std::lock_guard<std::mutex> lk(m_mtx);
    // Resume from ANY state, not only from Paused. Requiring an exact prior
    // state made resume a silent no-op whenever the state had moved on for
    // some other reason, which left the picture frozen with nothing in the log.
    if (m_head_set.load()) {
        m_play = PlayState::Playing;
    } else {
        // Never started: leave it Stopped so the host's start() path runs and
        // establishes the head properly.
        m_play = PlayState::Stopped;
    }
}

void DecoderSession::jump_to_live() {
    std::lock_guard<std::mutex> lk(m_mtx);
    uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
    uint64_t want = (m_latest_seq.load() > back) ? (m_latest_seq.load() - back)
                                          : m_first_available_seq.load();
    if (!m_head_set.load() || m_head.load() != std::max(want, m_first_available_seq.load())) {
        ++m_discontinuity;
        m_init_sent = false;        // decoder restarts, so it needs init again
    }
    m_head = std::max(want, m_first_available_seq.load());
    m_head_set = true;
    if (m_play == PlayState::Paused) m_play = PlayState::Playing;
}

bool DecoderSession::seek(uint64_t seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // Only within what the store still retains.
    if (seq < m_first_available_seq.load() || seq > m_latest_seq.load()) return false;
    if (!m_head_set.load() || seq != m_head.load()) {
        ++m_discontinuity;
        m_init_sent = false;        // decoder restarts, so it needs init again
    }
    m_head = seq;
    m_head_set = true;
    return true;
}

std::optional<PlayableSegment> DecoderSession::next_segment() {
    // Reading a segment is a multi-megabyte disk read, so it happens with no
    // lock held. Holding the state lock across it blocked the UI every six
    // seconds — which is what made clicking the timeline appear to lock OBS up.
    uint64_t want = 0;
    bool need_init = false;
    PlayableSegment out;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_play.load() != PlayState::Playing || !m_head_set.load())
            return std::nullopt;

        // Never serve past the live edge: the head must not run off the end of
        // what the encoder has actually published.
        if (m_head.load() > m_latest_seq.load()) return std::nullopt;

        want = m_head.load();
        if (!m_cache->has(want)) {
            // Waiting on a segment: hold position rather than skipping, so
            // nothing is silently dropped from the programme.
            m_stats.gaps_waited++;
            return std::nullopt;
        }

        out.seq = want;
        out.duration_s = m_segment_duration_s.load();
        for (const auto& sg : m_manifest.segments) {
            if (sg.seq != want) continue;
            if (sg.duration_s > 0.1) out.duration_s = sg.duration_s;
            if (sg.at_ms > 0) out.starts_at_ms = sg.at_ms;
        }
        if (out.starts_at_ms == 0 && m_started_at_ms.load() > 0)
            out.starts_at_ms = m_started_at_ms.load() +
                (int64_t)((double)want * m_segment_duration_s.load() * 1000.0);
        out.skip_to_ms = m_pending_skip_ms;
        m_pending_skip_ms = 0;          // applies to this segment only
        need_init = !m_init_sent;
    }

    // ── unlocked: the actual disk reads ──────────────────────────────────────
    auto media = m_cache->load(want);
    if (!media) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stats.gaps_waited++;
        return std::nullopt;
    }
    out.media = std::move(*media);

    std::vector<uint8_t> init;
    if (need_init) {
        if (auto i = m_cache->load_init()) init = std::move(*i);
    }

    // ── commit ───────────────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        // Re-check: the position may have moved while the file was read (a
        // seek, or a jump to live). If so, discard this one rather than
        // serving content from the old position.
        if (m_head.load() != want) return std::nullopt;
        if (need_init && !init.empty()) {
            out.init = std::move(init);
            m_init_sent = true;
        }
        ++m_head;
        m_stats.served++;
    }
    return out;
}

std::string DecoderSession::last_error() const {
    std::lock_guard<std::mutex> lk(m_err_mtx);
    return m_last_error;
}

std::vector<Marker> DecoderSession::markers() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_markers.markers;
}

bool DecoderSession::jump_to_marker(const std::string& marker_id) {
    uint64_t target = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (const auto& mk : m_markers.markers)
            if (mk.id == marker_id) { target = mk.seq; found = true; break; }
    }
    if (!found) return false;
    return seek(target);          // seek() bounds-checks and raises a jump
}

std::optional<Marker> DecoderSession::current_marker() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::optional<Marker> best;
    const uint64_t head = m_head.load();
    for (const auto& mk : m_markers.markers)
        if (mk.seq <= head && (!best || mk.seq >= best->seq)) best = mk;
    return best;
}

std::vector<AudioTrack> DecoderSession::audio_layout() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_manifest.audio_tracks;
}

uint64_t DecoderSession::discontinuity_id() const {
    return m_discontinuity.load();      // read by the feed loop constantly
}

// The estimate needs no lock at all, and is exact whenever segments are evenly
// spaced. Only try the manifest for a precise value, and use try_lock so a UI
// query is never blocked by the download thread.
int64_t DecoderSession::wall_clock_ms(uint64_t seq) const {
    const int64_t started = m_started_at_ms.load();
    if (std::unique_lock<std::mutex> lk(m_mtx, std::try_to_lock); lk.owns_lock()) {
        for (const auto& s : m_manifest.segments)
            if (s.seq == seq && s.at_ms > 0) return s.at_ms;
    }
    if (started <= 0) return 0;
    return started + (int64_t)((double)seq * m_segment_duration_s.load() * 1000.0);
}

int64_t DecoderSession::end_wall_ms() const {
    const uint64_t last = m_latest_seq.load();
    const int64_t at = wall_clock_ms(last);
    if (at <= 0) return 0;
    return at + (int64_t)(m_segment_duration_s.load() * 1000.0);
}

bool DecoderSession::at_end() const {
    return m_head_set.load() && m_head.load() > m_latest_seq.load();
}

int64_t DecoderSession::playhead_wall_ms() const {
    if (!m_head_set.load()) return 0;
    const int64_t t = wall_clock_ms(m_head.load());
    // Once playback runs past the last segment the head points at a position
    // that does not exist, and the reported time ran beyond the end of the
    // recording. Clamp it: the displayed time must never exceed what was
    // actually recorded.
    const int64_t end = end_wall_ms();
    if (end > 0 && t > end) return end;
    return t;
}

int64_t DecoderSession::live_wall_ms() const {
    return wall_clock_ms(m_latest_seq.load());
}

int64_t DecoderSession::earliest_wall_ms() const {
    return wall_clock_ms(m_first_available_seq.load());
}

int64_t DecoderSession::event_started_ms() const {
    return m_started_at_ms.load();
}

double DecoderSession::behind_live_s() const {
    // Lock-free: read by the UI several times a second.
    if (!m_head_set.load()) return 0.0;
    const uint64_t head = m_head.load(), live = m_latest_seq.load();
    if (live < head) return 0.0;
    return (double)(live - head) * m_segment_duration_s.load();
}

std::vector<std::pair<uint64_t, uint64_t>> DecoderSession::cached_ranges() const {
    auto seqs = m_cache->cached_seqs();
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (uint64_t s : seqs) {
        if (!out.empty() && s == out.back().second + 1) out.back().second = s;
        else out.push_back({ s, s });
    }
    return out;
}

// Seek by TIME, which is how an operator thinks. Finds the segment containing
// the requested moment and records how far into it to start, so accuracy is not
// limited to the segment boundary.
int64_t DecoderSession::seek_to_wall_ms(int64_t wall_ms) {
    uint64_t target = 0;
    int64_t  seg_start = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_manifest.started_at_ms <= 0) return 0;
        const double seg = m_segment_duration_s.load() > 0.1 ? m_segment_duration_s.load() : 6.0;

        // Prefer an exact match from the manifest window.
        bool found = false;
        for (const auto& s : m_manifest.segments) {
            if (s.at_ms <= 0) continue;
            const int64_t end = s.at_ms + (int64_t)(s.duration_s * 1000.0);
            if (wall_ms >= s.at_ms && wall_ms < end) {
                target = s.seq; seg_start = s.at_ms; found = true; break;
            }
        }
        if (!found) {
            // Outside the window: derive from the event start.
            const int64_t offset = wall_ms - m_manifest.started_at_ms;
            if (offset < 0) return 0;
            target = (uint64_t)((double)offset / 1000.0 / seg);
            seg_start = m_manifest.started_at_ms +
                        (int64_t)((double)target * seg * 1000.0);
        }
        if (target < m_first_available_seq.load() || target > m_latest_seq.load()) return 0;
    }

    if (!seek(target)) return 0;              // seek() raises the discontinuity
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_pending_skip_ms = wall_ms - seg_start;
        if (m_pending_skip_ms < 0) m_pending_skip_ms = 0;
    }
    return wall_ms;
}

double DecoderSession::buffered_ahead_s() const {
    // Lock-free apart from one copy of the cache index. The original version
    // called has() up to ten thousand times — each a filesystem check — from
    // the UI thread.
    if (!m_head_set.load()) return 0.0;
    const uint64_t head = m_head.load();
    const double seg = m_segment_duration_s.load();
    const auto idx = m_cache->cached_seqs();
    int n = 0;
    for (uint64_t s = head; idx.count(s); ++s) ++n;
    return (double)n * seg;
}

} // namespace multisite
