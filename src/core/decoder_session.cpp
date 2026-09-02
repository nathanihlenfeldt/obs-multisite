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
    std::lock_guard<std::mutex> lk(m_mtx);
    const int64_t now = now_override ? now_override : now_ms();

    // 1. Which event is live in this room?
    auto lp = m_tx.get("rooms/" + m_cfg.room_id + "/live.json");
    if (!lp.success) {
        m_last_error = "live.json: HTTP " + std::to_string(lp.http_status) +
                       " " + lp.error;
        m_room = RoomState::Offline;
        return m_room;
    }

    LivePointer live;
    try {
        live = LivePointer::from_json(
            std::string(lp.body.begin(), lp.body.end()));
    } catch (...) {
        m_last_error = "live.json is not valid JSON";
        m_room = RoomState::Offline;
        return m_room;
    }

    if (live.event_id.empty()) {
        m_room = RoomState::Offline;
        return m_room;
    }

    // Switching events resets the cache: a new event has its own init segment
    // and sequence space.
    if (live.event_id != m_event_id) {
        m_event_id = live.event_id;
        m_cache->set_event(m_event_id);
        m_head_set = false;
        m_init_sent = false;
        m_play = PlayState::Stopped;
        ++m_discontinuity;          // new event: new init segment and timeline
    }

    // 2. Fetch the rolling manifest.
    auto mf = m_tx.get(event_prefix() + "manifest.json");
    if (!mf.success) {
        m_last_error = "manifest.json: HTTP " + std::to_string(mf.http_status) +
                       " " + mf.error;
        m_room = RoomState::Offline;
        return m_room;
    }
    try {
        m_manifest = Manifest::from_json(
            std::string(mf.body.begin(), mf.body.end()));
    } catch (...) {
        m_last_error = "manifest.json is not valid JSON";
        m_room = RoomState::Offline;
        return m_room;
    }

    m_latest_seq          = m_manifest.latest_seq;
    m_first_available_seq = m_manifest.first_available_seq;
    m_manifest_updated_ms = m_manifest.updated_at_ms;
    if (m_manifest.stream_duration_hint() > 0.1)
        m_segment_duration_s = m_manifest.stream_duration_hint();

    // 3. Stale detection: an encoder that died leaves live.json pointing at an
    // event whose manifest stops advancing. Don't poll a corpse forever.
    const int64_t age = now - m_manifest_updated_ms;
    if (m_manifest.status == "ended") {
        m_room = RoomState::Ended;
    } else if (m_manifest_updated_ms > 0 && age > m_cfg.stale_after_ms) {
        m_last_error = "manifest last updated " + std::to_string(age / 1000) +
                       "s ago — treating the room as offline";
        m_room = RoomState::Offline;
    } else {
        m_room = RoomState::Live;
        m_last_error.clear();
    }
    return m_room;
}

bool DecoderSession::ensure_init() {
    if (m_cache->has_init()) return true;
    auto r = m_tx.get(event_prefix() + "init.mp4");
    if (!r.success) {
        m_last_error = "init.mp4: HTTP " + std::to_string(r.http_status) +
                       " " + r.error;
        return false;
    }
    return m_cache->store_init(r.body);
}

// ── Downloading ──────────────────────────────────────────────────────────────
bool DecoderSession::download_one(uint64_t seq) {
    auto r = m_tx.get(segment_key(seq));
    if (!r.success) {
        m_stats.download_failures++;
        m_last_error = "segment " + std::to_string(seq) + ": HTTP " +
                       std::to_string(r.http_status) + " " + r.error;
        return false;
    }
    const std::string want = checksum_for(seq);
    if (!m_cache->store(seq, r.body, want)) {
        // A checksum mismatch means corruption in flight: don't cache it, so
        // the next pump re-fetches instead of feeding bad data to the decoder.
        m_stats.checksum_failures++;
        m_last_error = "segment " + std::to_string(seq) +
                       " failed checksum verification";
        return false;
    }
    m_stats.downloaded++;
    return true;
}

int DecoderSession::pump_downloads(int max) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_event_id.empty()) return 0;
    if (!ensure_init()) return 0;

    // Download-ahead window starts at the head (or the live edge minus the
    // prebuffer, before playback has started) and runs forward. This is what
    // keeps the cache filling while paused.
    uint64_t from;
    if (m_head_set) {
        from = m_head;
    } else {
        uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
        from = (m_latest_seq > back) ? (m_latest_seq - back) : m_first_available_seq;
    }
    from = std::max(from, m_first_available_seq);

    uint64_t to = std::min<uint64_t>(
        m_latest_seq,
        from + (uint64_t)std::max(1, m_cfg.download_ahead_segments));

    int fetched = 0;
    for (uint64_t s = from; s <= to && fetched < max; ++s) {
        if (m_cache->has(s)) continue;
        if (download_one(s)) ++fetched;
    }

    // Bound disk use, but keep plenty of scrub-back room.
    if (m_head_set && m_head > (uint64_t)m_cfg.keep_behind_segments)
        m_cache->prune_below(m_head - (uint64_t)m_cfg.keep_behind_segments);

    return fetched;
}

// ── Playback ─────────────────────────────────────────────────────────────────
bool DecoderSession::start() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_room != RoomState::Live && m_room != RoomState::Ended) return false;
    if (!m_cache->has_init()) return false;

    if (!m_head_set) {
        // Start `prebuffer_segments` behind the live edge so there's a cushion.
        uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
        uint64_t want = (m_latest_seq > back) ? (m_latest_seq - back)
                                              : m_first_available_seq;
        want = std::max(want, m_first_available_seq);
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
    // so resuming loses nothing.
    if (m_play == PlayState::Playing) m_play = PlayState::Paused;
}

void DecoderSession::resume() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_play == PlayState::Paused) m_play = PlayState::Playing;
}

void DecoderSession::jump_to_live() {
    std::lock_guard<std::mutex> lk(m_mtx);
    uint64_t back = (uint64_t)std::max(0, m_cfg.prebuffer_segments);
    uint64_t want = (m_latest_seq > back) ? (m_latest_seq - back)
                                          : m_first_available_seq;
    if (!m_head_set || m_head != std::max(want, m_first_available_seq)) {
        ++m_discontinuity;
        m_init_sent = false;        // decoder restarts, so it needs init again
    }
    m_head = std::max(want, m_first_available_seq);
    m_head_set = true;
    if (m_play == PlayState::Paused) m_play = PlayState::Playing;
}

bool DecoderSession::seek(uint64_t seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // Only within what the store still retains.
    if (seq < m_first_available_seq || seq > m_latest_seq) return false;
    if (!m_head_set || seq != m_head) {
        ++m_discontinuity;
        m_init_sent = false;        // decoder restarts, so it needs init again
    }
    m_head = seq;
    m_head_set = true;
    return true;
}

std::optional<PlayableSegment> DecoderSession::next_segment() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_play != PlayState::Playing || !m_head_set) return std::nullopt;

    if (!m_cache->has(m_head)) {
        // Waiting on a segment: hold position rather than skipping, so nothing
        // is silently dropped from the programme.
        m_stats.gaps_waited++;
        return std::nullopt;
    }

    auto media = m_cache->load(m_head);
    if (!media) { m_stats.gaps_waited++; return std::nullopt; }

    PlayableSegment out;
    out.seq = m_head;
    out.duration_s = m_segment_duration_s;
    for (const auto& s : m_manifest.segments)
        if (s.seq == m_head && s.duration_s > 0.1) out.duration_s = s.duration_s;
    out.media = std::move(*media);

    if (!m_init_sent) {
        auto init = m_cache->load_init();
        if (init) out.init = std::move(*init);
        m_init_sent = true;
    }

    ++m_head;
    m_stats.served++;
    return out;
}

uint64_t DecoderSession::discontinuity_id() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_discontinuity;
}

double DecoderSession::behind_live_s() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_head_set || m_latest_seq < m_head) return 0.0;
    return (double)(m_latest_seq - m_head) * m_segment_duration_s;
}

double DecoderSession::buffered_ahead_s() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_head_set) return 0.0;
    uint64_t s = m_head;
    int n = 0;
    while (m_cache->has(s) && n < 10000) { ++s; ++n; }
    return (double)n * m_segment_duration_s;
}

} // namespace multisite
