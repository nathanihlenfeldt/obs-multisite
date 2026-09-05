#pragma once
//
// decoder_session.h — the satellite's receive-and-play controller.
//
// Implements timeslipping: the PLAYBACK HEAD is independent of the LIVE EDGE.
// Downloads run ahead into the local cache regardless of where playback sits,
// so a campus can pause (to hold for its own welcome), sit deliberately behind
// live, scrub backwards, or jump to live — without losing anything.
//
// Deliberately driven by explicit calls (poll / pump_downloads / advance)
// rather than hidden threads, so the whole state machine is testable
// deterministically. The OBS layer wraps this in threads.
//
#include "model.h"
#include "segment_cache.h"
#include "transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multisite {

// NOTE: the OBS layer carries this across as an int, so new states are
// appended rather than inserted.
enum class RoomState {
    Unknown,    // nothing fetched yet
    Offline,    // nothing to play: no live event, or nothing could be fetched
    Live,       // event is live and updating
    Ended,      // the encoder finished cleanly
    Interrupted,// the encoder died mid-service: still marked live, not advancing
};

// Whether a state means "this event is not growing any more, so play it as
// video-on-demand". A recording and an interrupted service differ in how they
// should be DESCRIBED, not in how they play.
inline bool is_vod(RoomState s) {
    return s == RoomState::Ended || s == RoomState::Interrupted;
}

enum class PlayState { Stopped, Playing, Paused };

struct DecoderConfig {
    std::string room_id = "main-auditorium";
    std::string cache_dir;
    // Segments to buffer before playback starts. Higher = more resilient.
    int    prebuffer_segments = 2;
    // How far ahead of the playhead to keep downloading, in MINUTES of
    // programme. This is the reliability figure that matters: it is how long
    // the campus could keep broadcasting if its connection died. Downloading
    // runs as fast as the link allows until this much is banked, rather than
    // trickling along at playback speed — which is what made jumping back
    // twenty minutes appear not to buffer at all.
    int    buffer_minutes = 10;
    // Hard ceiling, so a small disk cannot be filled by a long buffer.
    int    max_cached_segments = 2000;
    // Keep this many segments behind the head on disk (scrub-back room).
    int    keep_behind_segments = 200;
    // Treat the room as no-longer-live if the manifest hasn't updated within
    // this.
    int    stale_after_ms = 600000;      // 10 minutes
    int    max_download_retries = 5;
    // Play this specific event instead of whatever live.json names. Set when
    // an operator picks a past service from the event list; empty means
    // "follow the room", which is the live behaviour.
    std::string pinned_event_id;
};

// A decoded-ready fragment handed to the host: init + media, in order.
struct PlayableSegment {
    uint64_t seq = 0;
    double   duration_s = 6.0;
    std::vector<uint8_t> init;      // only populated on the first hand-off
    std::vector<uint8_t> media;
    // Wall-clock time of the start of this segment, so the host can report the
    // playing time precisely rather than per-segment.
    int64_t  starts_at_ms = 0;
    // Milliseconds into this segment at which playback should begin. Set after
    // a seek to a time that falls mid-segment; the host drops earlier frames.
    // Segments are the unit of transfer, but they need not be the unit of
    // seeking.
    int64_t  skip_to_ms = 0;
};

class DecoderSession {
public:
    DecoderSession(DecoderConfig cfg, Transport& transport);

    // ── Discovery ────────────────────────────────────────────────────────────
    // Fetch live.json + manifest.json. Call periodically (e.g. every 3s).
    // `now_ms_override` exists for deterministic tests.
    RoomState poll(int64_t now_ms_override = 0);

    RoomState room_state() const { return m_room.load(); }
    const std::string& event_id() const { return m_event_id; }

    // ── Pinning ──────────────────────────────────────────────────────────────
    // Play one specific event and stop following the room. Pinning an event
    // that is not the live one is how a past service is watched.
    //
    // A pinned session deliberately does NOT switch when a new service starts:
    // being yanked out of a recording someone is watching, because a rehearsal
    // began in the room, would be far worse than staying put. live_elsewhere()
    // reports that something is on air so the host can offer the jump instead
    // of taking it.
    void pin_event(const std::string& event_id);
    void unpin();                       // follow the room again
    std::string pinned_event() const;
    bool is_pinned() const;

    // The event live.json currently names, whether or not it is being played.
    std::string live_event_id() const;
    // True when the room is live but a different event is pinned.
    bool live_elsewhere() const;
    // Why the last operation failed. Returned by value under its own small
    // lock, so it never contends with the download path.
    std::string last_error() const;

    // ── Downloading ──────────────────────────────────────────────────────────
    // Fetch up to `max` missing segments in the download-ahead window.
    // Returns how many were newly cached. Runs independently of playback, so
    // the cache keeps filling while paused.
    int pump_downloads(int max = 4);

    // ── Playback (timeslipping) ──────────────────────────────────────────────
    bool start();                      // begins once prebuffer is satisfied
    void pause();                      // freezes the head; cache keeps filling
    void resume();                      // continues from the paused position
    void jump_to_live();               // snap the head to the live edge
    bool seek(uint64_t seq);           // move within what's retained

    // Hand the next segment to the decoder, if one is due and cached.
    std::optional<PlayableSegment> next_segment();

    PlayState  play_state()  const { return m_play.load(); }
    uint64_t   playback_head() const { return m_head.load(); }
    uint64_t   live_edge()    const { return m_latest_seq.load(); }
    uint64_t   earliest_available() const { return m_first_available_seq.load(); }

    // ── Markers ──────────────────────────────────────────────────────────────
    // Cues published by the main site (markers.json), refreshed on poll.
    std::vector<Marker> markers() const;

    // Move playback to a marker. Returns false if the marker is unknown or its
    // segment is no longer retained.
    bool jump_to_marker(const std::string& marker_id);

    // The marker at or before the playback head — i.e. "where are we in the
    // service", for display.
    std::optional<Marker> current_marker() const;

    // Increments whenever playback jumps (seek, jump-to-live, event change).
    // The host must tear down and restart its decoder when this changes: the
    // next fragment will carry an unrelated baseMediaDecodeTime, which would
    // otherwise decode as out-of-order timestamps and a glitched picture.
    uint64_t discontinuity_id() const;

    // What the main site says its audio contains: track labels and, for a
    // packed multi-channel feed, what each channel carries. Without this the
    // names the encoder operator typed would never be seen by anyone.
    std::vector<AudioTrack> audio_layout() const;

    // A finished recording behaves as video-on-demand: it has an end, a
    // duration, and a position within it — "behind live" is meaningless.
    bool    event_ended() const { return is_vod(m_room.load()); }
    // Distinguishes the two ways an event stops: ended cleanly, or the encoder
    // died. Both play; only the wording differs.
    bool    was_interrupted() const { return m_room.load() == RoomState::Interrupted; }
    // Whether this event was seen LIVE at any point since it was loaded.
    // "The broadcast just ended" and "this is a recording of a past service"
    // are different things to an operator, and only this distinguishes them.
    bool    was_live_this_session() const { return m_saw_live.load(); }
    // Wall-clock time just past the last frame of the recording.
    int64_t end_wall_ms() const;
    // True once playback has run past the last segment there is.
    bool    at_end() const;

    // Wall-clock time of a position in the programme. Uses the exact time
    // recorded for a segment when it is still in the manifest window, and
    // estimates from the event start otherwise. 0 if unknown.
    int64_t wall_clock_ms(uint64_t seq) const;
    int64_t playhead_wall_ms() const;
    int64_t live_wall_ms() const;
    int64_t earliest_wall_ms() const;
    int64_t event_started_ms() const;

    // How far behind live the campus currently is, in seconds.
    double behind_live_s() const;

    // Seconds of contiguous cached content ahead of the head — i.e. how long
    // playback could continue with no network at all.
    double buffered_ahead_s() const;

    // The contiguous cached ranges, as [first,last] sequence pairs, so a UI can
    // draw what is actually on disk rather than approximate it.
    std::vector<std::pair<uint64_t, uint64_t>> cached_ranges() const;

    // Seek to a wall-clock time. Returns the exact position reached, or 0 if
    // the time is outside what storage still retains. Sub-segment accuracy is
    // handled by the host: `skip_to_ms` in the served segment tells it how far
    // into that segment to begin.
    int64_t seek_to_wall_ms(int64_t wall_ms);

    const SegmentCache& cache() const { return *m_cache; }

    struct Stats {
        uint64_t downloaded = 0;
        uint64_t download_failures = 0;
        uint64_t checksum_failures = 0;
        uint64_t served = 0;
        uint64_t gaps_waited = 0;
    };
    const Stats& stats() const { return m_stats; }

private:
    DecoderConfig m_cfg;
    Transport&    m_tx;
    std::unique_ptr<SegmentCache> m_cache;

    std::string m_event_id;
    std::string m_pinned_event_id;     // guarded by m_mtx
    std::string m_live_event_id;       // what live.json last named
    std::atomic<bool> m_room_is_live{false};   // …and whether it was advancing
    std::atomic<RoomState> m_room{RoomState::Unknown};
    std::atomic<PlayState> m_play{PlayState::Stopped};
    std::string m_last_error;
    mutable std::mutex m_err_mtx;      // guards m_last_error only

    Manifest   m_manifest;
    MarkerList m_markers;
    int64_t    m_markers_checked_ms = 0;
    int64_t    m_manifest_updated_ms = 0;

    // The values the UI reads many times a second are atomics, not
    // mutex-protected fields.
    //
    // Keeping the lock off the network path was not enough on Windows: the
    // download thread acquires the lock in a tight loop, and Windows mutexes
    // favour the thread already running, so the UI thread was starved for
    // seconds at a time (measured at 12.6 s). Reads that only need a scalar
    // now take no lock at all, which removes the contention rather than
    // hoping to win the race for it.
    std::atomic<uint64_t> m_latest_seq{0};
    std::atomic<uint64_t> m_first_available_seq{0};
    std::atomic<double>   m_segment_duration_s{6.0};
    std::atomic<int64_t>  m_started_at_ms{0};
    std::atomic<bool>     m_saw_live{false};
    std::atomic<uint64_t> m_head{0};
    std::atomic<bool>     m_head_set{false};
    bool     m_init_sent = false;
    std::atomic<uint64_t> m_discontinuity{0};
    int64_t  m_pending_skip_ms = 0;   // applied to the next served segment

    Stats m_stats;
    mutable std::mutex m_mtx;

    std::string event_prefix() const;
    std::string segment_key(uint64_t seq) const;
    std::string checksum_for(uint64_t seq) const;

};

} // namespace multisite
