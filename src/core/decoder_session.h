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

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multisite {

enum class RoomState {
    Unknown,    // nothing fetched yet
    Offline,    // no live event, or the manifest has gone stale
    Live,       // event is live and updating
    Ended,      // the encoder finished cleanly
};

enum class PlayState { Stopped, Playing, Paused };

struct DecoderConfig {
    std::string room_id = "main-auditorium";
    std::string cache_dir;
    // Segments to buffer before playback starts. Higher = more resilient.
    int    prebuffer_segments = 2;
    // How far ahead of the playback head to keep downloading.
    int    download_ahead_segments = 10;
    // Keep this many segments behind the head on disk (scrub-back room).
    int    keep_behind_segments = 200;
    // Treat the room as Offline if the manifest hasn't updated within this.
    int    stale_after_ms = 600000;      // 10 minutes
    int    max_download_retries = 5;
};

// A decoded-ready fragment handed to the host: init + media, in order.
struct PlayableSegment {
    uint64_t seq = 0;
    double   duration_s = 6.0;
    std::vector<uint8_t> init;      // only populated on the first hand-off
    std::vector<uint8_t> media;
};

class DecoderSession {
public:
    DecoderSession(DecoderConfig cfg, Transport& transport);

    // ── Discovery ────────────────────────────────────────────────────────────
    // Fetch live.json + manifest.json. Call periodically (e.g. every 3s).
    // `now_ms_override` exists for deterministic tests.
    RoomState poll(int64_t now_ms_override = 0);

    RoomState room_state() const { return m_room; }
    const std::string& event_id() const { return m_event_id; }
    const std::string& last_error() const { return m_last_error; }

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

    PlayState  play_state()  const { return m_play; }
    uint64_t   playback_head() const { return m_head; }
    uint64_t   live_edge()    const { return m_latest_seq; }
    uint64_t   earliest_available() const { return m_first_available_seq; }

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

    // How far behind live the campus currently is, in seconds.
    double behind_live_s() const;

    // Seconds of contiguous cached content ahead of the head.
    double buffered_ahead_s() const;

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
    RoomState   m_room = RoomState::Unknown;
    PlayState   m_play = PlayState::Stopped;
    std::string m_last_error;

    Manifest   m_manifest;
    MarkerList m_markers;
    int64_t    m_markers_checked_ms = 0;
    uint64_t m_latest_seq = 0;
    uint64_t m_first_available_seq = 0;
    double   m_segment_duration_s = 6.0;
    int64_t  m_manifest_updated_ms = 0;

    uint64_t m_head = 0;
    bool     m_head_set = false;
    bool     m_init_sent = false;
    uint64_t m_discontinuity = 0;

    Stats m_stats;
    mutable std::mutex m_mtx;

    std::string event_prefix() const;
    std::string segment_key(uint64_t seq) const;
    std::string checksum_for(uint64_t seq) const;
    bool ensure_init();
    bool download_one(uint64_t seq);
};

} // namespace multisite
