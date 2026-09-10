// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
//
// storage_manager.h — encoder-side storage management: list the room's events
// with their sizes, and delete them (one at a time or older-than-N) with a
// verification pass so the operator gets a real confirmation rather than a
// hope.
//
// Retention is still a bucket lifecycle rule by default; this is the explicit,
// on-demand complement for when an operator wants a specific event gone now.
//
#include "event_catalog.h"
#include "transport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace multisite {

// One event in the storage report.
struct ManagedEvent {
    std::string event_id;
    std::string name;
    int64_t     started_at_ms = 0;
    EventState  state = EventState::Unknown;
    bool        is_live = false;   // live.json currently names it — cannot delete
    uint64_t    objects = 0;       // objects under events/{id}/
    uint64_t    bytes = 0;         // summed sizes (0 when the store omitted them)
    // Whether the two figures above were actually MEASURED. A listing that fails
    // leaves them at zero, and zero is exactly what an empty event looks like —
    // so a window that showed them anyway would tell an operator that an event
    // holding gigabytes held nothing, and invite them to delete it.
    bool        size_known = false;
    std::string size_error;        // why not, when size_known is false
};

// How a listing went, for the log and for the window's own summary. The core
// keeps no log of its own, so the caller decides what to say with this.
struct ListStats {
    int     requests = 0;          // HTTP requests issued
    int     events = 0;
    int     tallies_failed = 0;
    int64_t elapsed_ms = 0;
    bool    cancelled = false;
};

struct DeleteReport {
    bool        ok = false;
    uint64_t    objects_deleted = 0;
    uint64_t    bytes_freed = 0;
    bool        confirmed = false; // the event prefix re-listed empty afterwards
    std::string error;
};

class StorageManager {
public:
    StorageManager(std::string room_id, Transport& transport);

    // The room's events: id, name, start time, state, live flag. No size work at
    // all, which is the whole point of it — this is the part that has to appear
    // while an operator is still looking at the window. Returns false on a
    // listing/permission failure, with `error` set.
    bool list_events(std::vector<ManagedEvent>& out, std::string& error,
                     ListStats* stats = nullptr,
                     const std::atomic<bool>* cancel = nullptr);

    // The objects and bytes under one event, measured. Deliberately separate
    // from the listing above because it is the expensive half: it lists every
    // object the event holds, which for a three-hour event at six-second
    // segments is two thousand keys and several pages of request.
    //
    // Reports failure in `error` and leaves `size_known` false, rather than
    // leaving a zero that reads as "empty".
    bool tally_size(ManagedEvent& e, std::string& error,
                    ListStats* stats = nullptr,
                    const std::atomic<bool>* cancel = nullptr);

    // Both, one event after another. Kept for callers that want the whole answer
    // in one go rather than progressively.
    bool list(std::vector<ManagedEvent>& out, std::string& error,
              ListStats* stats = nullptr,
              const std::atomic<bool>* cancel = nullptr);

    // Permanently delete one event and its room-index entry. Refuses the event
    // live.json currently names. `progress(done, total)` runs as objects go;
    // returning false cancels. Verifies by re-listing the event prefix.
    DeleteReport delete_event(
        const std::string& event_id,
        const std::function<bool(uint64_t, uint64_t)>& progress = {});

    // Delete every event older than `older_than_days` (by start time). The
    // live event is always kept. Returns a combined report.
    DeleteReport delete_older_than(
        int older_than_days, int64_t now_ms,
        const std::function<bool(uint64_t, uint64_t)>& progress = {});

private:
    std::string  m_room_id;
    Transport&   m_tx;
    EventCatalog m_catalog;

    // All object keys under `prefix` and their summed sizes, following
    // pagination. Returns false on failure, on cancellation, or when a prefix
    // refuses to finish paging.
    bool list_prefix(const std::string& prefix, std::vector<std::string>& keys,
                     uint64_t& bytes, std::string& error,
                     ListStats* stats = nullptr,
                     const std::atomic<bool>* cancel = nullptr);

    // The event live.json currently names, or "" (none / unreadable).
    std::string live_event_id();

    // Delete one key. A 404 counts as success (already gone).
    bool remove_key(const std::string& key, std::string& error);
};

} // namespace multisite
