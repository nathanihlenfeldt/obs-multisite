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

    // Enumerate the room's events with names, sizes and live status. Returns
    // false on a listing/permission failure, with `error` set.
    bool list(std::vector<ManagedEvent>& out, std::string& error);

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
    // pagination. Returns false on failure.
    bool list_prefix(const std::string& prefix, std::vector<std::string>& keys,
                     uint64_t& bytes, std::string& error);

    // The event live.json currently names, or "" (none / unreadable).
    std::string live_event_id();

    // Delete one key. A 404 counts as success (already gone).
    bool remove_key(const std::string& key, std::string& error);
};

} // namespace multisite
