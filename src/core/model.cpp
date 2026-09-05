#include "model.h"
#include "../vendor/nlohmann/json.hpp"

using json = nlohmann::json;

namespace multisite {

static json video_to_json(const VideoInfo& v) {
    return { {"codec", v.codec}, {"width", v.width}, {"height", v.height}, {"fps", v.fps} };
}
static VideoInfo video_from_json(const json& j) {
    VideoInfo v;
    v.codec  = j.value("codec", "h264");
    v.width  = j.value("width", 0);
    v.height = j.value("height", 0);
    v.fps    = j.value("fps", 0.0);
    return v;
}
static json tracks_to_json(const std::vector<AudioTrack>& ts) {
    json a = json::array();
    for (const auto& t : ts) {
        json e = { {"idx", t.idx}, {"label", t.label}, {"codec", t.codec},
                   {"channels", t.channels}, {"sample_rate", t.sample_rate} };
        if (!t.channel_labels.empty()) e["channel_labels"] = t.channel_labels;
        a.push_back(e);
    }
    return a;
}
static std::vector<AudioTrack> tracks_from_json(const json& j) {
    std::vector<AudioTrack> out;
    if (!j.is_array()) return out;
    for (const auto& t : j) {
        AudioTrack a;
        a.idx        = t.value("idx", 0);
        a.label      = t.value("label", "");
        a.codec      = t.value("codec", "aac");
        a.channels   = t.value("channels", 2);
        a.sample_rate= t.value("sample_rate", 48000);
        if (t.contains("channel_labels") && t["channel_labels"].is_array())
            for (const auto& cl : t["channel_labels"])
                a.channel_labels.push_back(cl.get<std::string>());
        out.push_back(a);
    }
    return out;
}

// ── LivePointer ───────────────────────────────────────────────────────────────
std::string LivePointer::to_json() const {
    json j = { {"room_id", room_id}, {"event_id", event_id},
               {"status", status}, {"updated_at_ms", updated_at_ms} };
    return j.dump();
}
LivePointer LivePointer::from_json(const std::string& s) {
    json j = json::parse(s);
    LivePointer p;
    p.room_id       = j.value("room_id", "");
    p.event_id      = j.value("event_id", "");
    p.status        = j.value("status", "live");
    p.updated_at_ms = j.value("updated_at_ms", (int64_t)0);
    return p;
}

// ── EventInfo ─────────────────────────────────────────────────────────────────
std::string EventInfo::to_json() const {
    json j;
    j["event_id"]           = event_id;
    j["room_id"]            = room_id;
    j["started_at_ms"]      = started_at_ms;
    j["first_seq"]          = first_seq;
    j["segment_duration_s"] = segment_duration_s;
    j["init"]               = init;
    j["video"]              = video_to_json(video);
    j["audio_tracks"]       = tracks_to_json(audio_tracks);
    return j.dump();
}
EventInfo EventInfo::from_json(const std::string& s) {
    json j = json::parse(s);
    EventInfo e;
    e.event_id           = j.value("event_id", "");
    e.room_id            = j.value("room_id", "");
    e.started_at_ms      = j.value("started_at_ms", (int64_t)0);
    e.first_seq          = j.value("first_seq", (uint64_t)0);
    e.segment_duration_s = j.value("segment_duration_s", 6.0);
    e.init               = j.value("init", "init.mp4");
    if (j.contains("video")) e.video = video_from_json(j["video"]);
    if (j.contains("audio_tracks")) e.audio_tracks = tracks_from_json(j["audio_tracks"]);
    return e;
}

// ── Object-key layout ─────────────────────────────────────────────────────────
std::string live_pointer_key(const std::string& room_id) {
    return "rooms/" + room_id + "/live.json";
}
std::string room_events_prefix(const std::string& room_id) {
    return "rooms/" + room_id + "/events/";
}
std::string room_event_key(const std::string& room_id, const std::string& event_id) {
    return room_events_prefix(room_id) + event_id + ".json";
}
std::string event_prefix_for(const std::string& event_id) {
    return "events/" + event_id + "/";
}

std::string event_id_from_index_key(const std::string& key) {
    // Accepts "rooms/{room}/events/{id}.json" (an index key) or
    // "events/{id}/" (a common prefix from listing the flat namespace).
    if (key.empty()) return "";
    size_t last = key.find_last_not_of('/');
    if (last == std::string::npos) return "";
    const bool had_slash = (last + 1 < key.size());
    size_t start = key.find_last_of('/', last);
    std::string tail = (start == std::string::npos)
                     ? key.substr(0, last + 1)
                     : key.substr(start + 1, last - start);
    if (!had_slash) {
        // An index key ends in .json; a prefix does not.
        const std::string ext = ".json";
        if (tail.size() > ext.size() &&
            tail.compare(tail.size() - ext.size(), ext.size(), ext) == 0)
            tail = tail.substr(0, tail.size() - ext.size());
        else
            return "";
    }
    return tail;
}

// ── RoomEventEntry ────────────────────────────────────────────────────────────
std::string RoomEventEntry::to_json() const {
    json j = { {"event_id", event_id}, {"room_id", room_id},
               {"started_at_ms", started_at_ms} };
    return j.dump();
}
RoomEventEntry RoomEventEntry::from_json(const std::string& s) {
    json j = json::parse(s);
    RoomEventEntry e;
    e.event_id      = j.value("event_id", "");
    e.room_id       = j.value("room_id", "");
    e.started_at_ms = j.value("started_at_ms", (int64_t)0);
    return e;
}

// ── Manifest ──────────────────────────────────────────────────────────────────
void Manifest::push(const ManifestSegment& s, size_t window) {
    // De-duplicate: a crash between publishing the manifest and clearing the
    // spool causes a harmless re-upload, which must not double-list the segment.
    for (auto& existing : segments) {
        if (existing.seq == s.seq) {
            existing = s;                       // refresh in place
            if (s.seq > latest_seq) latest_seq = s.seq;
            return;
        }
    }
    segments.push_back(s);
    if (s.seq > latest_seq) latest_seq = s.seq;
    while (segments.size() > window) segments.erase(segments.begin());
    window_start_seq = segments.empty() ? 0 : segments.front().seq;
}

double Manifest::stream_duration_hint() const {
    // Median-ish: just take the last listed segment's duration, falling back to
    // the first. Segment durations vary slightly with keyframe placement.
    if (!segments.empty()) {
        if (segments.back().duration_s > 0.1) return segments.back().duration_s;
        if (segments.front().duration_s > 0.1) return segments.front().duration_s;
    }
    return 0.0;
}

std::string Manifest::to_json() const {
    json j;
    j["event_id"]            = event_id;
    j["status"]              = status;
    j["updated_at_ms"]       = updated_at_ms;
    j["started_at_ms"]       = started_at_ms;
    j["first_available_seq"] = first_available_seq;
    j["window_start_seq"]    = window_start_seq;
    j["latest_seq"]          = latest_seq;
    j["init"]                = init;
    j["video"]               = video_to_json(video);
    j["audio_tracks"]        = tracks_to_json(audio_tracks);
    json segs = json::array();
    for (const auto& s : segments)
        segs.push_back({ {"seq", s.seq}, {"duration_s", s.duration_s},
                         {"checksum", s.checksum}, {"at_ms", s.at_ms} });
    j["segments"] = segs;
    return j.dump();
}
Manifest Manifest::from_json(const std::string& s) {
    json j = json::parse(s);
    Manifest m;
    m.event_id            = j.value("event_id", "");
    m.status              = j.value("status", "live");
    m.updated_at_ms       = j.value("updated_at_ms", (int64_t)0);
    m.started_at_ms       = j.value("started_at_ms", (int64_t)0);
    m.first_available_seq = j.value("first_available_seq", (uint64_t)0);
    m.window_start_seq    = j.value("window_start_seq", (uint64_t)0);
    m.latest_seq          = j.value("latest_seq", (uint64_t)0);
    m.init                = j.value("init", "init.mp4");
    if (j.contains("video")) m.video = video_from_json(j["video"]);
    if (j.contains("audio_tracks")) m.audio_tracks = tracks_from_json(j["audio_tracks"]);
    if (j.contains("segments") && j["segments"].is_array()) {
        for (const auto& seg : j["segments"]) {
            ManifestSegment ms;
            ms.seq        = seg.value("seq", (uint64_t)0);
            ms.duration_s = seg.value("duration_s", 6.0);
            ms.checksum   = seg.value("checksum", "");
            ms.at_ms      = seg.value("at_ms", (int64_t)0);
            m.segments.push_back(ms);
        }
    }
    return m;
}

// ── Markers ───────────────────────────────────────────────────────────────────
std::string MarkerList::to_json() const {
    json arr = json::array();
    for (const auto& mk : markers)
        arr.push_back({ {"seq", mk.seq}, {"at_ms", mk.at_ms},
                        {"type", mk.type}, {"label", mk.label}, {"id", mk.id} });
    return json({ {"markers", arr} }).dump();
}
MarkerList MarkerList::from_json(const std::string& s) {
    json j = json::parse(s);
    MarkerList ml;
    if (j.contains("markers") && j["markers"].is_array()) {
        for (const auto& mk : j["markers"]) {
            Marker m;
            m.seq   = mk.value("seq", (uint64_t)0);
            m.at_ms = mk.value("at_ms", (int64_t)0);
            m.type  = mk.value("type", "cue");
            m.label = mk.value("label", "");
            m.id    = mk.value("id", "");
            ml.markers.push_back(m);
        }
    }
    return ml;
}

} // namespace multisite
