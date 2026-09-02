#pragma once
//
// session.h — the publishing layer.
//
// Ties the CMAF muxer → durable spool → retrying uploader → manifest/live
// pointer together, and enforces the protocol's write-ordering invariant:
// a segment is only ever listed in manifest.json AFTER the object store has
// confirmed it durable. If a decoder can see a manifest entry, the segment
// exists.
//
// Object layout written by a session:
//   rooms/{room_id}/live.json
//   events/{event_id}/event.json
//   events/{event_id}/init.mp4
//   events/{event_id}/segments/{seq:08d}.m4s
//   events/{event_id}/manifest.json
//   events/{event_id}/markers.json
//
#include "model.h"
#include "spool_queue.h"
#include "retry_uploader.h"
#include "transport.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace multisite {

struct SessionConfig {
    std::string room_id = "main-auditorium";
    std::string spool_dir;                 // local durable queue location
    double      segment_duration_s = 6.0;
    size_t      manifest_window = 50;      // rolling window size
    // Object tagging: S3 supports it, but Cloudflare R2 does NOT and rejects
    // requests carrying x-amz-tagging. R2 users should instead configure a
    // bucket lifecycle rule by prefix/age. Default off for broad compatibility.
    bool        use_object_tags = false;
    std::string expiry_tag_key = "MultisiteExpiry";
    std::string expiry_tag_val = "7d";
    int         heartbeat_interval_s = 10; // live.json refresh cadence
    // Upload retry tuning (production defaults; tests shorten these).
    int         base_backoff_ms = 250;
    int         max_backoff_ms  = 15000;
    double      backoff_jitter  = 0.30;
};

// Generates a lexicographically-sortable, time-prefixed unique event id
// (ULID-style: 10 chars of time + 16 of randomness, Crockford base32).
std::string make_event_id(int64_t now_ms = 0);

int64_t now_ms();

class Session {
public:
    Session(SessionConfig cfg, Transport& transport);
    ~Session();

    // Is there an unfinished event on disk that could be resumed?
    ResumeInfo check_resumable() const;

    // Start a new event. Publishes event.json + init.mp4, then live.json.
    // `init` is the CMAF init segment; tracks describe what's inside segments.
    bool start_new(const std::vector<uint8_t>& init,
                   const VideoInfo& video,
                   const std::vector<AudioTrack>& audio_tracks);

    // Resume the on-disk event, continuing its sequence numbering.
    bool resume(const std::vector<uint8_t>& init,
                const VideoInfo& video,
                const std::vector<AudioTrack>& audio_tracks);

    // Durably enqueue a finished media fragment. Returns its sequence number.
    // Safe to call from the encode thread; never blocks on the network.
    uint64_t publish_segment(std::vector<uint8_t> fragment,
                             double duration_s,
                             double pts_offset_s);

    // Append a marker and publish markers.json.
    void add_marker(const std::string& label, const std::string& type = "cue");

    // Refresh live.json's heartbeat (drives decoder stale-detection).
    void heartbeat();

    // Flush remaining spool, mark the event ended, publish final state.
    void end(std::chrono::milliseconds drain_deadline = std::chrono::milliseconds(30000));

    // Status for the encoder UI.
    struct Status {
        std::string event_id;
        uint64_t    last_confirmed = 0;
        uint64_t    last_enqueued  = 0;
        size_t      pending        = 0;
        uint64_t    confirmed_total = 0;
        uint64_t    bytes_uploaded = 0;
        uint64_t    retries        = 0;
        uint64_t    verify_failures = 0;
        std::string verify_note;      // result of the last upload verification
        LinkHealth  health = LinkHealth::Healthy;
    };
    Status status() const;

    // Called after every segment is confirmed durable, so the host can report
    // progress (segment count, bytes, pending depth, link health).
    using ProgressCallback = std::function<void(const Status&)>;
    void set_progress_callback(ProgressCallback cb) { m_on_progress = std::move(cb); }

    // Bytes confirmed uploaded so far (feeds OBS's own output stats).
    uint64_t bytes_uploaded() const;

    const std::string& event_id() const { return m_event_id; }

    // Why the last operation failed (HTTP status + body). Empty if none.
    const std::string& last_error() const { return m_last_error; }

private:
    SessionConfig m_cfg;
    Transport&    m_tx;
    std::unique_ptr<SpoolQueue>    m_spool;
    std::unique_ptr<RetryUploader> m_uploader;

    std::string m_event_id;
    uint64_t    m_next_seq = 0;
    int64_t     m_last_heartbeat_ms = 0;

    Manifest    m_manifest;
    MarkerList  m_markers;
    std::string m_last_error;
    ProgressCallback m_on_progress;
    mutable std::mutex m_mtx;

    std::string segment_key(uint64_t seq) const;
    std::string event_prefix() const;
    bool  put_json(const std::string& key, const std::string& body);
    bool  put_bytes(const std::string& key, const std::vector<uint8_t>& b,
                    const std::string& content_type);
    void  publish_manifest_locked();
    void  publish_live(const std::string& status);
    bool  begin_common(const std::vector<uint8_t>& init,
                       const VideoInfo& video,
                       const std::vector<AudioTrack>& tracks);
    void  on_confirmed(const SpooledSegment& seg);
};

} // namespace multisite
