#pragma once
//
// event_catalog.h — what a satellite can play, and what state each event is in.
//
// Listing gives IDs; it does not give state. An operator picking from a list
// needs to know whether a thing is on air now, finished, or was cut off — so
// each event's manifest is read and classified.
//
// Like DecoderSession, this is driven by explicit calls rather than hidden
// threads, so the state machine is deterministically testable. Unlike
// DecoderSession, one refresh() makes a request per event: it MUST run on a
// worker thread, never on the UI thread.
//
#include "model.h"
#include "transport.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace multisite {

enum class EventState {
    Unknown,      // listed, but its manifest could not be read
    Live,         // on air now: the manifest is advancing
    Recording,    // the encoder finished cleanly — plays as VOD
    Interrupted,  // still marked live, but nothing has updated it for too long
};

const char* to_string(EventState s);

struct EventSummary {
    std::string event_id;
    std::string room_id;
    int64_t     started_at_ms = 0;
    int64_t     last_update_ms = 0;      // manifest.updated_at_ms
    EventState  state = EventState::Unknown;
    uint64_t    latest_seq = 0;
    uint64_t    first_available_seq = 0;
    // Programme length, from the manifest's own segment times. An estimate for
    // a live event, since it is still growing.
    double      duration_s = 0.0;
    // Whether the live pointer currently names this event. An INTERRUPTED event
    // is often still pointed at, which is exactly the confusing case.
    bool        is_live_pointer = false;
};

struct CatalogConfig {
    std::string room_id = "main-auditorium";
    // The same rule the decoder uses to call a room offline, so the list and
    // the player never disagree about whether something is still running.
    int stale_after_ms = 600000;         // 10 minutes
    // Cap on events examined per refresh. 0 means every retained event; with
    // 7-day retention that is a couple of dozen. A cap only matters if
    // retention is later extended.
    int max_events = 0;
};

class EventCatalog {
public:
    EventCatalog(CatalogConfig cfg, Transport& transport);

    // Enumerate and classify. Returns false if the listing itself failed —
    // which is different from "this room has no events", and must be shown
    // differently. `now_ms_override` exists for deterministic tests.
    bool refresh(int64_t now_ms_override = 0);

    // Newest first. Copied out under the lock: the UI reads this while a
    // refresh may be running on the worker.
    std::vector<EventSummary> events() const;

    std::string last_error() const;

    // True when the room index was empty and the flat events/ namespace had to
    // be scanned instead — the path for events recorded before the index
    // existed. Costs an extra request per event, so it is worth surfacing.
    bool used_fallback_scan() const;

    // Events listed whose manifest could not be read, and which were therefore
    // left out. Non-zero means the list is showing less than the store holds.
    int skipped() const;

private:
    CatalogConfig m_cfg;
    Transport&    m_tx;

    mutable std::mutex m_mtx;
    std::vector<EventSummary> m_events;
    std::string m_last_error;
    bool m_fallback = false;
    int  m_skipped = 0;

    // Every event id in the room, newest last. Uses the room index, falling
    // back to a scan of events/ when the index yields nothing.
    bool collect_event_ids(std::vector<std::string>& out, bool& fallback,
                           std::string& error);
    bool classify(const std::string& event_id, const LivePointer& live,
                  int64_t now, EventSummary& out);
};

} // namespace multisite
