// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "splash.h"

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
    // Operator-facing title from the encoder's event name; empty when the
    // event predates naming or the encoder left it blank.
    std::string name;
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
    // Connection health, 0 healthy / 1 degraded / 2 offline, and whether a
    // request has been observed yet. Mirrors the OBS dock's meaning.
    int         link_health = 0;
    bool        link_known = false;
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

    // ── Is the storage reachable, from where, and how fast? ─────────────────
    // The three questions asked when a campus stutters, and the ones nothing
    // here could answer: an operator could see the picture was wrong but not
    // whether the bucket was reachable, which Cloudflare edge was serving it,
    // or what the link was managing.
    struct StorageHealth {
        bool        configured = false;   // credentials and a feed name are set
        std::string endpoint;
        std::string bucket;
        std::string room;
        bool        reachable = false;    // the endpoint answered at all
        bool        readable = false;     // …and our key could read the feed
        long        http_status = 0;
        std::string error;                // empty when readable
        int64_t     round_trip_ms = 0;
        std::string colo;                 // Cloudflare edge, e.g. "JNB"
        std::string server;               // the Server header
        double      bytes_per_s = 0.0;    // observed, from real segment traffic
        uint64_t    rate_samples = 0;     // 0 means show no figure at all
    };
    // `probe` issues one read-only request for the room's live pointer; without
    // it only the passively-observed figures are filled in, which is what a
    // status poll several times a minute should use.
    StorageHealth storage_health(bool probe);

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
    // Makes sure the sound card is open, is open on the device the settings now
    // name, and is open at the rate the feed actually runs at. Called both when
    // a frame arrives and when the box is idle — see the note at its
    // definition for why the idle case is not an optimisation.
    void ensure_audio_open(const Config& cfg, int feed_channels, int feed_rate);

    // Puts the idle screen up when there is no programme going out, and
    // takes it down again when there is. Driven from the poll loop rather
    // than a timer of its own: it only ever needs to act a few times a
    // minute, and never while frames are flowing.
    void update_screen();
    // Everything the identity screen needs to say right now.
    SplashInfo splash_info() const;

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
    // Split by stream. A dropped picture repeats the last one; a dropped audio
    // frame is audible. The total could not say which was happening, and which
    // one climbs is the difference between the card stalling and the decoder
    // falling behind.
    std::atomic<uint64_t> m_dropped_video{0}, m_dropped_audio{0};
    // How long present() actually takes. The delivery thread scales the
    // picture and waits for the vertical blank inline, so if the display path
    // is slow it does not just make the picture late — it throttles the whole
    // playout clock, and the symptom shows up as "playout fell behind".
    // Totals since the last status line; read and reset there.
    std::atomic<uint64_t> m_present_ns{0}, m_present_max_ns{0}, m_presents{0};
    // Read only by the poll thread, which is the only writer too.
    uint64_t m_last_frames_out = 0;
    // Cleared when a decoder is created, set by the first frame out of it.
    std::atomic<bool> m_logged_stream{true};
    // Said once per run, not once per discarded frame.
    std::atomic<bool> m_logged_audio_tracks{false};
    // Monotonic time of the last frame that reached the display. What
    // separates "playing" from "nothing is arriving", which is the difference
    // between leaving the picture alone and putting the splash back up.
    std::atomic<uint64_t> m_last_frame_ns{0};
    // What the idle screen currently says. Re-rendering identical text would
    // page-flip the display for no reason.
    std::string m_idle_signature;
    bool        m_idle_showing = false;
    // Boot splash: show the identity screen for the first few seconds even if
    // auto-play is about to put an event on the screen.
    std::atomic<uint64_t> m_boot_splash_until_ns{0};
    std::atomic<bool>     m_boot_splash_drawn{false};
    // The sound card is opened by the delivery thread, once the first decoded
    // frame reveals the feed's rate and channel count. Setting this asks it to
    // let the current device go and open the newly chosen one — otherwise a
    // device picked in the interface would not take effect until the next
    // reboot, which for a box with no keyboard is no use at all.
    std::atomic<bool> m_audio_open{false};
    std::atomic<bool> m_audio_reopen{false};
    // The rate the feed last reported, and the rate the card was actually
    // opened at. The first is what lets the device be opened before anything
    // has played — a box sitting idle between services still has to put its
    // sound on the network — and the second is what catches a feed that turns
    // out not to be the assumed 48 kHz, so it is reopened rather than played
    // back at the wrong pitch.
    std::atomic<int> m_audio_rate{0};
    std::atomic<int> m_audio_opened_rate{0};

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
