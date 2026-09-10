// SPDX-License-Identifier: GPL-3.0-or-later
// test_storage_manager.cpp — encoder-side storage management.
//
// Proves the dangerous part in isolation: which events are listed, that
// deleting one removes every object under events/{id}/ plus its room-index
// entry while leaving live.json alone, that the live event is refused, and
// that "older than N days" only touches the events it should.
#include "../src/core/storage_manager.h"
#include "../src/core/model.h"

#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// An in-memory store that supports put/get/list/remove, enough for the manager.
class MemStore : public Transport {
public:
    std::map<std::string, std::string> objects;

    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string,std::string>&) override {
        objects[key] = std::string(body.begin(), body.end());
        PutResult r; r.success = true; r.http_status = 200; return r;
    }
    GetResult get(const std::string& key) override {
        GetResult r;
        auto it = objects.find(key);
        if (it == objects.end()) { r.http_status = 404; r.error = "not found"; return r; }
        r.success = true; r.http_status = 200;
        r.body.assign(it->second.begin(), it->second.end());
        return r;
    }
    ListResult list(const std::string& prefix, const std::string& delimiter,
                    const std::string& token, int) override {
        ListResult r;
        size_t start = token.empty() ? 0 : (size_t)std::stoul(token);
        size_t seen = 0;
        for (const auto& [key, val] : objects) {
            if (key.compare(0, prefix.size(), prefix) != 0) continue;
            if (seen++ < start) continue;
            if (!delimiter.empty()) {
                size_t d = key.find(delimiter, prefix.size());
                if (d != std::string::npos) {
                    std::string cp = key.substr(0, d + delimiter.size());
                    if (r.common_prefixes.empty() || r.common_prefixes.back() != cp)
                        r.common_prefixes.push_back(cp);
                    continue;
                }
            }
            ListEntry e; e.key = key; e.size = (int64_t)val.size();
            r.keys.push_back(e);
        }
        r.success = true; r.http_status = 200;
        return r;
    }
    DeleteResult remove(const std::string& key) override {
        DeleteResult r;
        auto it = objects.find(key);
        if (it == objects.end()) { r.success = true; r.http_status = 404; return r; }
        objects.erase(it);
        r.success = true; r.http_status = 204;
        return r;
    }
};

static void make_event(MemStore& s, const std::string& room, const std::string& id,
                       int64_t started_ms, const std::string& status,
                       int n_segments, const std::string& name) {
    EventInfo ev;
    ev.event_id = id; ev.room_id = room; ev.started_at_ms = started_ms; ev.name = name;
    s.objects[event_prefix_for(id) + "event.json"] = ev.to_json();

    RoomEventEntry ix;
    ix.event_id = id; ix.room_id = room; ix.started_at_ms = started_ms; ix.name = name;
    s.objects[room_event_key(room, id)] = ix.to_json();

    Manifest m;
    m.event_id = id; m.status = status; m.name = name;
    m.started_at_ms = started_ms; m.updated_at_ms = started_ms;
    m.latest_seq = (uint64_t)(n_segments > 0 ? n_segments - 1 : 0);
    ManifestSegment seg; seg.seq = 0; seg.duration_s = 6.0; seg.at_ms = started_ms;
    m.segments.push_back(seg);
    s.objects[event_prefix_for(id) + "manifest.json"] = m.to_json();

    s.objects[event_prefix_for(id) + "init.mp4"] = std::string(1200, 'i');
    for (int i = 0; i < n_segments; ++i) {
        char buf[16]; std::snprintf(buf, sizeof(buf), "%08d", i);
        s.objects[event_prefix_for(id) + "segments/" + buf + ".m4s"] =
            std::string(4096, 's');
    }
}

static void set_live(MemStore& s, const std::string& room, const std::string& id) {
    LivePointer lp;
    lp.room_id = room; lp.event_id = id; lp.status = "live"; lp.updated_at_ms = 0;
    s.objects[live_pointer_key(room)] = lp.to_json();
}

int main() {
    const int64_t NOW = 1'757'000'000'000LL;
    const int64_t DAY = 24LL * 60 * 60 * 1000;
    const std::string room = "main";

    std::printf("== 1. Lists the room's events with sizes and live status ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW - 2 * DAY, "ended", 3, "Alpha");
        make_event(s, room, "01BBB", NOW - DAY, "ended", 2, "Beta");
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        std::vector<ManagedEvent> evs;
        std::string err;
        CHECK(mgr.list(evs, err), "list succeeds");
        CHECK(evs.size() == 3, "all three events listed");
        if (evs.size() == 3) {
            CHECK(evs[0].event_id == "01CCC" && evs[0].is_live,
                  "the live event is first and marked live");
            CHECK(evs[0].name == "Gamma", "the live event carries its name");
            CHECK(evs[0].objects == 4 && evs[0].bytes > 0,
                  "sizes tallied (descriptor + init + manifest + 1 segment)");
            CHECK(evs[2].event_id == "01AAA" && !evs[2].is_live,
                  "a finished event is listed and not live");
        }
    }

    std::printf("== 2. Delete removes the event and its index entry ==\n");
    {
        MemStore s;
        make_event(s, room, "01AAA", NOW - 2 * DAY, "ended", 3, "Alpha");
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_event("01AAA");
        CHECK(r.ok, "delete succeeds");
        CHECK(r.objects_deleted == 7, "all 6 media objects plus the index entry removed");
        CHECK(r.confirmed, "verification re-list confirmed the prefix is empty");
        CHECK(s.objects.find("events/01AAA/init.mp4") == s.objects.end(),
              "event media is gone");
        CHECK(s.objects.find("rooms/main/events/01AAA.json") == s.objects.end(),
              "the room index entry is gone");
        CHECK(s.objects.find("rooms/main/live.json") != s.objects.end(),
              "live.json is left alone");
        CHECK(s.objects.find("events/01CCC/init.mp4") != s.objects.end(),
              "the other event is untouched");
    }

    std::printf("== 3. The live event is refused ==\n");
    {
        MemStore s;
        make_event(s, room, "01CCC", NOW, "live", 1, "Gamma");
        set_live(s, room, "01CCC");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_event("01CCC");
        CHECK(!r.ok, "deleting the live event is refused");
        CHECK(r.error.find("on air") != std::string::npos, "and says why");
        CHECK(s.objects.find("events/01CCC/init.mp4") != s.objects.end(),
              "nothing was actually deleted");
    }

    std::printf("== 4. Older-than-N deletes only the old events ==\n");
    {
        MemStore s;
        make_event(s, room, "01OLD", NOW - 10 * DAY, "ended", 2, "Old");
        make_event(s, room, "01RECENT", NOW - 3 * DAY, "ended", 2, "Recent");
        make_event(s, room, "01LIVE", NOW, "live", 1, "Live");
        set_live(s, room, "01LIVE");

        StorageManager mgr(room, s);
        DeleteReport r = mgr.delete_older_than(7, NOW);
        CHECK(r.ok, "cleanup succeeds");
        CHECK(r.objects_deleted == 6, "the old event (5 objects + index) was removed");
        CHECK(s.objects.find("events/01OLD/init.mp4") == s.objects.end(),
              "the old event is gone");
        CHECK(s.objects.find("events/01RECENT/init.mp4") != s.objects.end(),
              "a recent event inside the window is kept");
        CHECK(s.objects.find("events/01LIVE/init.mp4") != s.objects.end(),
              "the live event is kept");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL STORAGE MANAGER TESTS PASSED"
                                       : "SOME STORAGE MANAGER TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}

