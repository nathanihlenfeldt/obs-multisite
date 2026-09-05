#pragma once
//
// player.h — the appliance's engine: everything between the bucket and the
// HDMI socket.
//
// This is the headless twin of the OBS source. It owns exactly the same
// receive core — S3Transport, DecoderSession, EventCatalog, CmafDecoder — and
// differs only in where decoded frames end up: an HDMI output the box drives
// itself, rather than frames handed to OBS.
//
// The threading model is the one the OBS source arrived at, because the
// reasons for it were learned the hard way:
//
//   poll     — refreshes live.json / manifest.json and drives download-ahead
//   feed     — hands cached fragments to the decoder in order
//   decode   — inside CmafDecoder, emits frames
//   deliver  — releases frames when they are due, in presentation order
//
// Pacing happens on the deliver thread and nowhere else. Video and audio share
// the decode thread, so sleeping inside a decoder callback would delay every
// frame decoded after it.
//
// Pause is enforced at DELIVERY, not at feeding: by the time a fragment is fed
// its whole six seconds are already decoded, so gating the feed would let the
// picture run on for a segment after the operator pressed Hold.
//
#include "config.h"
#include "audio_output.h"
#include "video_output.h"

#include "../core/decoder_session.h"
#include "../core/event_catalog.h"
#include "../core/cmaf_decoder.h"
#include "../core/s3_transport.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_player {

// One row of the event list, in the plain terms the UI needs.
struct EventEntry {
    std::string event_id;
    long long   started_ms = 0;
    double      duration_s = 0;
    // 0 unknown, 1 live, 2 recording, 3 interrupted — matching
    // multisite::EventState, so the browser never sees a core enum.
    int         state = 0;
};

struct EventListing {
    std::vector<EventEntry> events;
    bool        loading = false;
    bool        listed_once = false;
    bool        fallback_scan = false;
    int         skipped = 0;
    std::string error;
};

// Everything the web UI draws, captured in one consistent read. The browser
// polls this; nothing else it asks for can disagree with it, because it all
// comes from the same snapshot.
struct Status {
    // ── What is on air ───────────────────────────────────────────────────────
    std::string room_id;
    int         room_state = 0;         // matches multisite::RoomState
    std::string event_id;
    std::string pinned_event_id;
    bool        live_elsewhere = false;
    std::string live_event_id;

    bool        playing = false;
    bool        paused = false;
    bool        buffering = false;      // playing, but nothing decoded yet
    bool        loading = false;        // switching events
    bool        locked = false;

    // ── Where in the programme ───────────────────────────────────────────────
    // Clock times throughout. The UI never mentions a sequence number.
    long long   playhead_ms = 0, live_ms = 0, earliest_ms = 0, started_ms = 0;
    long long   end_ms = 0, total_ms = 0;
    long long   seek_target_ms = 0;     // where playback is heading, if moving
    double      behind_live_s = 0.0;
    double      delay_from_live_s = 0.0;
    bool        ended = false;          // a recording, not a live feed
    bool        at_end = false;
    bool        was_live = false;       // seen live since it was loaded
    bool        interrupted = false;    // the encoder died rather than finished

    // ── Reliability readout ──────────────────────────────────────────────────
    double      buffered_ahead_s = 0.0;
    size_t      cached_segments = 0;
    std::vector<std::pair<long long, long long>> cached_spans;
    unsigned long long downloaded = 0, download_failures = 0;
    unsigned long long checksum_failures = 0, gaps_waited = 0;
    unsigned long long frames_out = 0, frames_dropped = 0;
    std::string last_error;

    // ── The feed's own description of itself ─────────────────────────────────
    int         video_width = 0, video_height = 0;
    int         audio_channels = 0;
    std::vector<std::string> channel_labels;

    struct MarkerEntry { std::string label; std::string id; long long at_ms; };
    std::vector<MarkerEntry> markers;
    std::string current_marker;

    // ── The box itself ───────────────────────────────────────────────────────
    bool        configured = false;     // storage credentials present
    std::string output_description;     // e.g. "HDMI-A-1 1920x1080@50"
    std::string audio_description;
    bool        video_output_ok = false;
    bool        audio_output_ok = false;
};

class Player {
public:
    Player(Config cfg, VideoOutput& video, AudioOutput& audio);
    ~Player();

    // Bring up the transport and the worker threads. Safe to call on a box
    // with no credentials yet: it simply reports itself unconfigured and waits
    // for somebody to fill them in over the web UI.
    void start();
    void stop();

    // Apply edited settings. Anything that changes what is being received
    // rebuilds the session; the picture goes away and comes back. Returns the
    // config actually in force.
    void reconfigure(const Config& cfg);
    Config config() const;

    // ── Operator controls (the same set the Qt dock offers) ──────────────────
    void play();
    void stop_playback();
    void pause();
    void resume();
    void toggle_pause();
    void jump_to_live();
    void seek_to_time(long long wall_ms);
    void jog(double seconds);
    void set_delay_from_live(double seconds);
    void jump_to_marker(const std::string& id);
    void pin_event(const std::string& event_id);
    void unpin_event();
    void set_locked(bool locked);
    bool locked() const { return m_locked.load(); }

    // ── Event list ───────────────────────────────────────────────────────────
    // Asks for a refresh on the worker. Listing plus a manifest per event is
    // far too much to do while a browser request waits.
    void refresh_events();
    void event_listing(EventListing& out) const;

    void status(Status& out) const;

    // The most recent decoded picture, for the preview. Deliberately separate
    // from the output path: the preview may be one frame a second, may lag,
    // and may be looked at while the output is held — lining up a cue is
    // exactly when those must not be the same thing.
    //
    // Returns false when nothing has been decoded yet. `version` lets a caller
    // wait for a frame it has not already sent.
    // The outputs in use, so the interface can list what this box can do
    // without a second path to the hardware.
    const VideoOutput& video() const { return m_video; }
    const AudioOutput& audio() const { return m_audio; }

    bool latest_frame(multisite::DecodedVideoFrame& out, uint64_t& version) const;
    uint64_t frame_version() const { return m_frame_version.load(); }

private:
    void poll_loop();
    void feed_loop();
    void deliver_loop();

    void rebuild_session();          // under m_obj_mtx
    void teardown_decoder();
    void flush_delivery();
    void note_error(const std::string& what);

    std::shared_ptr<multisite::DecoderSession> session_ref() const;
    std::shared_ptr<multisite::CmafDecoder>    decoder_ref() const;

    // One frame on its way to the output, already stamped with the monotonic
    // time it is due.
    struct PendingFrame {
        bool     is_video = true;
        uint64_t due_ns = 0;
        multisite::DecodedVideoFrame video;
        multisite::DecodedAudioFrame audio;
    };
    void enqueue(PendingFrame&& f);
    void on_video(const multisite::DecodedVideoFrame& f);
    void on_audio(const multisite::DecodedAudioFrame& f);
    int64_t anchor_pts(int64_t pts_ns, bool is_video);

    Config       m_cfg;
    mutable std::mutex m_cfg_mtx;
    VideoOutput& m_video;
    AudioOutput& m_audio;

    std::shared_ptr<multisite::S3Transport>    m_transport;
    std::shared_ptr<multisite::DecoderSession> m_session;
    std::shared_ptr<multisite::CmafDecoder>    m_decoder;
    std::shared_ptr<multisite::EventCatalog>   m_catalog;
    mutable std::mutex m_obj_mtx;

    std::thread m_poll_thread, m_feed_thread, m_deliver_thread;
    std::atomic<bool> m_running{false};

    // Playout clock: due time = base + (media pts − first media pts).
    std::atomic<uint64_t> m_playout_base_ns{0};
    std::atomic<int64_t>  m_first_pts_ns{-1};
    std::atomic<uint64_t> m_last_resync_log_ns{0};
    std::atomic<bool>     m_decoder_started{false};
    uint64_t              m_seen_discontinuity = 0;

    // An operator action asks for the next poll to happen now rather than
    // whenever the interval comes round: waiting out three seconds before even
    // looking is what makes Load feel like a dropped click.
    std::atomic<bool> m_poll_now{false};
    int      m_last_room = -1;
    uint64_t m_feed_start_ns = 0;      // monotonic time this decoder started
    uint64_t m_pushed_media_ns = 0;    // media duration handed over so far
    bool     m_logged_av_offset = false;
    int64_t  m_last_video_pts_ns = 0;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_locked{false};
    std::atomic<bool> m_flushing{false};
    std::atomic<bool> m_awaiting_frames{false};
    std::atomic<long long> m_seek_target_ms{0};
    std::atomic<bool>      m_loading_event{false};

    // Clock reading of the frame currently on screen, advanced per frame so
    // the displayed time moves continuously rather than once per segment.
    std::atomic<long long> m_playing_at_ms{0};
    std::atomic<long long> m_seg_starts_at_ms{0};
    std::atomic<int64_t>   m_seg_first_pts_ns{-1};
    std::atomic<int64_t>   m_skip_until_pts_ns{-1};

    std::deque<PendingFrame> m_dq;
    mutable std::mutex       m_dq_mtx;
    std::condition_variable  m_dq_cv;

    std::atomic<uint64_t> m_frames_out{0}, m_frames_dropped{0};

    // Latest decoded picture, kept for the preview.
    mutable std::mutex m_frame_mtx;
    multisite::DecodedVideoFrame m_last_frame;
    std::atomic<uint64_t> m_frame_version{0};

    mutable std::mutex m_events_mtx;
    EventListing       m_events;
    std::atomic<bool>  m_events_refreshing{false};
    std::atomic<bool>  m_events_wanted{false};

    mutable std::mutex m_err_mtx;
    std::string        m_last_error;
    std::atomic<double> m_delay_from_live_s{0.0};
};

} // namespace multisite_player
