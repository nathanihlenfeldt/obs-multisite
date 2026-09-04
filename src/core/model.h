#pragma once
//
// model.h — the storage-protocol data model: live pointer, event descriptor,
// rolling manifest, and markers. JSON (de)serialisation via nlohmann.
//
#include <string>
#include <vector>
#include <cstdint>

namespace multisite {

// rooms/{room}/live.json — points at the current live event.
struct LivePointer {
    std::string room_id;
    std::string event_id;
    std::string status = "live";     // "live" | "ended"
    int64_t     updated_at_ms = 0;    // heartbeat; drives decoder stale-detection
    std::string to_json() const;
    static LivePointer from_json(const std::string&);
};

struct AudioTrack {
    int         idx = 0;
    std::string label;
    std::string codec = "aac";
    int         channels = 2;
    int         sample_rate = 48000;
    // Packed multi-channel mode: channel ORDER is the interface between the
    // encoder and the satellite (channel 3 must be the click at both ends), so
    // the mapping is published rather than inferred. Positional names from the
    // speaker layout (FL/FR/LFE/...) are meaningless here and are ignored.
    // Empty for ordinary stereo tracks.
    std::vector<std::string> channel_labels;
};

struct VideoInfo {
    std::string codec = "h264";
    int width = 0, height = 0;
    double fps = 0.0;
};

// events/{event_id}/event.json — static descriptor written once at Go Live.
struct EventInfo {
    std::string event_id;
    std::string room_id;
    int64_t     started_at_ms = 0;
    uint64_t    first_seq = 0;
    double      segment_duration_s = 6.0;
    std::string init = "init.mp4";
    VideoInfo   video;
    std::vector<AudioTrack> audio_tracks;
    std::string to_json() const;
    static EventInfo from_json(const std::string&);
};

struct ManifestSegment {
    uint64_t    seq = 0;
    double      duration_s = 6.0;
    std::string checksum;            // sha256 hex of the segment
    // Wall-clock time of the CONTENT in this segment (event start plus its
    // offset in the programme), not the time it happened to be uploaded.
    // Operators think in clock time — "just after 10:42" — never in sequence
    // numbers, so this is what the UI shows.
    int64_t     at_ms = 0;
};

// events/{event_id}/manifest.json — rolling live-edge window.
struct Manifest {
    std::string event_id;
    std::string status = "live";
    int64_t     updated_at_ms = 0;
    // When this event started, so a satellite can convert any position to a
    // clock time even for segments outside the rolling window.
    int64_t     started_at_ms = 0;
    uint64_t    first_available_seq = 0;  // oldest still-retained (timeslip floor)
    uint64_t    window_start_seq = 0;     // oldest listed here
    uint64_t    latest_seq = 0;           // live edge
    std::string init = "init.mp4";
    VideoInfo   video;
    std::vector<AudioTrack>    audio_tracks;
    std::vector<ManifestSegment> segments;
    std::string to_json() const;
    static Manifest from_json(const std::string&);

    // Append a confirmed segment, trimming the window to `window` entries and
    // advancing latest_seq. Enforces the rolling window.
    void push(const ManifestSegment& s, size_t window);

    // Typical segment duration, taken from the listed segments (decoders use
    // it to estimate how far behind live they are). 0 if unknown.
    double stream_duration_hint() const;
};

struct Marker {
    uint64_t    seq = 0;
    int64_t     at_ms = 0;
    std::string type = "cue";
    std::string label;
    std::string id;
};

struct MarkerList {
    std::vector<Marker> markers;
    std::string to_json() const;
    static MarkerList from_json(const std::string&);
};

} // namespace multisite
