#include "event_catalog.h"

#include <algorithm>

namespace multisite {

int64_t now_ms();   // session.cpp

const char* to_string(EventState s) {
    switch (s) {
        case EventState::Live:        return "live";
        case EventState::Recording:   return "recording";
        case EventState::Interrupted: return "interrupted";
        default:                      return "unknown";
    }
}

EventCatalog::EventCatalog(CatalogConfig cfg, Transport& transport)
    : m_cfg(std::move(cfg)), m_tx(transport) {}

std::vector<EventSummary> EventCatalog::events() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_events;
}
std::string EventCatalog::last_error() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_last_error;
}
bool EventCatalog::used_fallback_scan() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_fallback;
}
int EventCatalog::skipped() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_skipped;
}

bool EventCatalog::collect_event_ids(std::vector<std::string>& out,
                                     bool& fallback, std::string& error) {
    out.clear();
    fallback = false;

    // ── The room index: one request per page, already room-scoped ────────────
    std::string token;
    do {
        ListResult r = m_tx.list(room_events_prefix(m_cfg.room_id), "", token, 1000);
        if (!r.success) {
            error = r.error;
            return false;
        }
        for (const auto& e : r.keys) {
            std::string id = event_id_from_index_key(e.key);
            if (!id.empty()) out.push_back(id);
        }
        token = r.truncated ? r.next_continuation_token : std::string();
    } while (!token.empty());

    if (!out.empty()) return true;

    // ── Fallback: events recorded before the index existed ───────────────────
    // The flat namespace has no room in the key, so every event.json must be
    // read to find out which room it belongs to. Expensive on purpose — it is
    // the compatibility path, not the normal one.
    fallback = true;
    token.clear();
    std::vector<std::string> candidates;
    do {
        ListResult r = m_tx.list("events/", "/", token, 1000);
        if (!r.success) {
            error = r.error;
            return false;
        }
        for (const auto& p : r.common_prefixes) {
            std::string id = event_id_from_index_key(p);
            if (!id.empty()) candidates.push_back(id);
        }
        token = r.truncated ? r.next_continuation_token : std::string();
    } while (!token.empty());

    // Newest first while filtering, so a max_events cap keeps the recent ones.
    // ULIDs order lexicographically by time, and a listing only ever returns
    // ascending order — there is no "newest first" to ask the store for.
    std::sort(candidates.begin(), candidates.end(), std::greater<std::string>());

    for (const auto& id : candidates) {
        if (m_cfg.max_events > 0 && (int)out.size() >= m_cfg.max_events) break;
        GetResult g = m_tx.get(event_prefix_for(id) + "event.json");
        if (!g.success) continue;
        try {
            EventInfo ev = EventInfo::from_json(std::string(g.body.begin(), g.body.end()));
            if (ev.room_id == m_cfg.room_id) out.push_back(id);
        } catch (...) {
            // A malformed descriptor is not a reason to abandon the listing.
        }
    }
    return true;
}

bool EventCatalog::classify(const std::string& event_id, const LivePointer& live,
                            int64_t now, EventSummary& out) {
    GetResult g = m_tx.get(event_prefix_for(event_id) + "manifest.json");
    if (!g.success) return false;

    Manifest m;
    try {
        m = Manifest::from_json(std::string(g.body.begin(), g.body.end()));
    } catch (...) {
        return false;
    }

    out.event_id            = event_id;
    out.room_id             = m_cfg.room_id;
    out.started_at_ms       = m.started_at_ms;
    out.last_update_ms      = m.updated_at_ms;
    out.latest_seq          = m.latest_seq;
    out.first_available_seq = m.first_available_seq;
    out.is_live_pointer     = (live.event_id == event_id);

    // Length from the manifest's own content times. The rolling window means
    // the listed segments are the tail of the programme, not all of it — but
    // the last one still tells us where the end is relative to the start.
    if (!m.segments.empty()) {
        const auto& last = m.segments.back();
        const int64_t end_ms = last.at_ms + (int64_t)(last.duration_s * 1000.0);
        if (end_ms > m.started_at_ms)
            out.duration_s = (double)(end_ms - m.started_at_ms) / 1000.0;
    }

    const int64_t age = now - m.updated_at_ms;
    if (m.status == "ended") {
        out.state = EventState::Recording;
    } else if (m.updated_at_ms > 0 && age > (int64_t)m_cfg.stale_after_ms) {
        // Still marked live, but nothing has touched it for too long: the
        // encoder died rather than ending. Without this the event would sit as
        // "live" forever and could never be played back — the one case where
        // you would most want to watch it again.
        out.state = EventState::Interrupted;
    } else {
        out.state = EventState::Live;
    }
    return true;
}

bool EventCatalog::refresh(int64_t now_override) {
    const int64_t now = now_override ? now_override : now_ms();

    // Which event is on air. A room that has never gone live has no pointer at
    // all, which is not an error — its past events still list.
    LivePointer live;
    GetResult lp = m_tx.get(live_pointer_key(m_cfg.room_id));
    if (lp.success) {
        try {
            live = LivePointer::from_json(std::string(lp.body.begin(), lp.body.end()));
        } catch (...) {
            live = LivePointer{};
        }
    }

    std::vector<std::string> ids;
    bool fallback = false;
    std::string error;
    if (!collect_event_ids(ids, fallback, error)) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_last_error = "could not list events: " + error;
        m_fallback = fallback;
        // The previous list is left in place: a transient listing failure
        // should not blank a list the operator is reading.
        return false;
    }

    // Newest first, so a cap keeps the events someone is most likely to want.
    std::sort(ids.begin(), ids.end(), std::greater<std::string>());
    if (m_cfg.max_events > 0 && (int)ids.size() > m_cfg.max_events)
        ids.resize(m_cfg.max_events);

    std::vector<EventSummary> found;
    int skipped = 0;
    for (const auto& id : ids) {
        EventSummary s;
        if (classify(id, live, now, s)) found.push_back(std::move(s));
        else ++skipped;      // listed but unreadable: retention caught it mid-flight
    }

    // Sort by the event's own start time, falling back to the id (which encodes
    // the time) when a manifest never recorded one.
    std::sort(found.begin(), found.end(),
              [](const EventSummary& a, const EventSummary& b) {
                  if (a.started_at_ms != b.started_at_ms)
                      return a.started_at_ms > b.started_at_ms;
                  return a.event_id > b.event_id;
              });

    // A live event belongs at the top whatever its start time says.
    std::stable_partition(found.begin(), found.end(),
                          [](const EventSummary& s) { return s.state == EventState::Live; });

    std::lock_guard<std::mutex> lk(m_mtx);
    m_events   = std::move(found);
    m_fallback = fallback;
    m_skipped  = skipped;
    m_last_error.clear();
    if (skipped > 0)
        m_last_error = std::to_string(skipped) +
                       " event(s) listed but not readable — retention may have "
                       "removed them";
    return true;
}

} // namespace multisite
