// SPDX-License-Identifier: GPL-3.0-or-later
#include "storage_manager.h"

#include <algorithm>
#include <utility>

namespace multisite {

StorageManager::StorageManager(std::string room_id, Transport& transport)
    : m_room_id(std::move(room_id)), m_tx(transport),
      m_catalog(CatalogConfig{m_room_id, 600000}, transport) {}

bool StorageManager::list(std::vector<ManagedEvent>& out, std::string& error) {
    if (!m_catalog.refresh()) {
        error = m_catalog.last_error();
        return false;
    }

    const std::string live = live_event_id();
    out.clear();
    for (const auto& s : m_catalog.events()) {
        ManagedEvent e;
        e.event_id      = s.event_id;
        e.name          = s.name;
        e.started_at_ms = s.started_at_ms;
        e.state         = s.state;
        e.is_live       = (!live.empty() && live == s.event_id);

        std::vector<std::string> keys;
        uint64_t bytes = 0;
        std::string err;
        if (list_prefix(event_prefix_for(s.event_id), keys, bytes, err)) {
            e.objects = keys.size();
            e.bytes   = bytes;
        }
        // A failed size tally must not hide the event; it lists with no size.
        out.push_back(std::move(e));
    }

    // Live first, then newest first.
    std::sort(out.begin(), out.end(), [](const ManagedEvent& a, const ManagedEvent& b) {
        if (a.is_live != b.is_live) return a.is_live;
        if (a.started_at_ms != b.started_at_ms)
            return a.started_at_ms > b.started_at_ms;
        return a.event_id > b.event_id;
    });
    return true;
}

bool StorageManager::list_prefix(const std::string& prefix,
                                 std::vector<std::string>& keys,
                                 uint64_t& bytes, std::string& error) {
    keys.clear();
    bytes = 0;
    std::string token;
    do {
        ListResult r = m_tx.list(prefix, "", token, 1000);
        if (!r.success) {
            error = r.error;
            return false;
        }
        for (const auto& e : r.keys) {
            keys.push_back(e.key);
            if (e.size > 0) bytes += (uint64_t)e.size;
        }
        token = r.truncated ? r.next_continuation_token : std::string();
    } while (!token.empty());
    return true;
}

std::string StorageManager::live_event_id() {
    GetResult g = m_tx.get(live_pointer_key(m_room_id));
    if (!g.success) return "";
    try {
        return LivePointer::from_json(
                   std::string(g.body.begin(), g.body.end())).event_id;
    } catch (...) {
        return "";
    }
}

bool StorageManager::remove_key(const std::string& key, std::string& error) {
    DeleteResult r = m_tx.remove(key);
    if (r.success) return true;
    error = "DELETE " + key + ": " +
            (r.error.empty() ? ("HTTP " + std::to_string(r.http_status)) : r.error);
    return false;
}

DeleteReport StorageManager::delete_event(
    const std::string& event_id,
    const std::function<bool(uint64_t, uint64_t)>& progress) {
    DeleteReport rep;

    const std::string live = live_event_id();
    if (!live.empty() && live == event_id) {
        rep.error = "That event is on air now and cannot be deleted.";
        return rep;
    }

    std::vector<std::string> keys;
    uint64_t bytes = 0;
    std::string err;
    if (!list_prefix(event_prefix_for(event_id), keys, bytes, err)) {
        rep.error = err.empty() ? "could not list the event" : err;
        return rep;
    }

    // The room-index entry lives outside events/{id}/; remove it too so the
    // event list stops advertising an event whose media is gone.
    keys.push_back(room_event_key(m_room_id, event_id));

    const uint64_t total = keys.size();
    for (uint64_t i = 0; i < keys.size(); ++i) {
        if (progress && !progress(i, total)) {
            rep.objects_deleted = i;
            rep.bytes_freed = bytes;
            rep.error = "cancelled";
            return rep;
        }
        std::string e;
        if (!remove_key(keys[i], e)) {
            rep.objects_deleted = i;
            rep.bytes_freed = bytes;
            rep.error = e;
            return rep;
        }
    }

    // Verification: the event prefix must now be empty.
    std::vector<std::string> remaining;
    uint64_t rem_bytes = 0;
    std::string verr;
    bool confirmed = false;
    if (list_prefix(event_prefix_for(event_id), remaining, rem_bytes, verr))
        confirmed = remaining.empty();

    rep.ok = true;
    rep.objects_deleted = total;
    rep.bytes_freed = bytes;
    rep.confirmed = confirmed;
    return rep;
}

DeleteReport StorageManager::delete_older_than(
    int older_than_days, int64_t now_ms,
    const std::function<bool(uint64_t, uint64_t)>& progress) {
    DeleteReport rep;

    std::vector<ManagedEvent> events;
    std::string err;
    if (!list(events, err)) {
        rep.error = err;
        return rep;
    }

    const int64_t cutoff = now_ms - (int64_t)older_than_days * 24LL * 60 * 60 * 1000;
    std::vector<std::string> targets;
    for (const auto& e : events)
        if (!e.is_live && e.started_at_ms > 0 && e.started_at_ms < cutoff)
            targets.push_back(e.event_id);

    bool any_confirmed = targets.empty();
    for (const auto& id : targets) {
        DeleteReport r = delete_event(id, progress);
        rep.objects_deleted += r.objects_deleted;
        rep.bytes_freed += r.bytes_freed;
        if (!r.ok) {
            if (!r.error.empty() && r.error != "cancelled") rep.error = r.error;
            return rep;
        }
        any_confirmed = any_confirmed && r.confirmed;
    }

    rep.ok = true;
    rep.confirmed = any_confirmed;
    return rep;
}

} // namespace multisite
