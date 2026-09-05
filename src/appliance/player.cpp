#include "player.h"
#include "log.h"
#include "sysinfo.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace multisite_player {

using namespace multisite;

namespace {

// The playout clock is monotonic, not wall time: a box that corrects its clock
// by NTP mid-service must not jump the picture.
uint64_t now_ns() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<nanoseconds>(
               steady_clock::now().time_since_epoch()).count();
}

long long now_ms() {
    return (long long)(now_ns() / 1000000ULL);
}

// How far ahead of its due time a frame may be released. Small, because the
// output has nowhere to buffer it — unlike OBS, which had its own queue.
constexpr uint64_t kMaxDeliveryLeadNs = 20000000ULL;      // 20 ms
// Frames waiting to go out. Video and audio arrive in decode order and are
// released in presentation order, so this only has to smooth small jitter. At
// 1080p an I420 frame is 3 MB, so 16 caps out around 50 MB.
constexpr size_t   kMaxQueuedFrames = 16;
// Past this much lateness the playout clock has drifted behind — normally a
// stall waiting for a segment. Re-anchor rather than dumping a backlog.
constexpr uint64_t kClockResyncThresholdNs = 2000000000ULL;   // 2 s
// Cushion when anchoring: covers the reordering window plus jitter.
constexpr uint64_t kPlayoutCushionNs = 500000000ULL;          // 500 ms
// Keep the decoder a couple of seconds ahead of the wall clock so the first
// frames of each fragment are never late, without running seconds ahead.
constexpr uint64_t kFeedLeadNs = 2500000000ULL;               // 2.5 s

} // namespace

Player::Player(Config cfg, VideoOutput& video, AudioOutput& audio)
    : m_cfg(std::move(cfg)), m_video(video), m_audio(audio) {
    m_locked = m_cfg.locked;
    m_delay_from_live_s = m_cfg.delay_from_live_s;
}

Player::~Player() { stop(); }

Config Player::config() const {
    std::lock_guard<std::mutex> lk(m_cfg_mtx);
    return m_cfg;
}

std::shared_ptr<DecoderSession> Player::session_ref() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_session;
}

std::shared_ptr<CmafDecoder> Player::decoder_ref() const {
    std::lock_guard<std::mutex> lk(m_obj_mtx);
    return m_decoder;
}

void Player::note_error(const std::string& what) {
    std::lock_guard<std::mutex> lk(m_err_mtx);
    m_last_error = what;
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void Player::rebuild_session() {
    Config cfg = config();

    m_transport.reset();
    m_session.reset();
    m_catalog.reset();

    if (!cfg.configured()) {
        plog_warn("no storage configured — open the web interface and enter "
                  "the bucket details");
        note_error("no storage configured");
        return;
    }

    S3Config s3;
    s3.endpoint_host     = cfg.endpoint_host;
    s3.r2_account_id     = cfg.r2_account_id;
    s3.bucket            = cfg.bucket;
    s3.access_key_id     = cfg.access_key_id;
    s3.secret_access_key = cfg.secret_access_key;
    s3.region            = cfg.region;
    m_transport = std::make_shared<S3Transport>(s3);

    DecoderConfig dc;
    dc.room_id              = cfg.room_id;
    dc.cache_dir            = cfg.cache_dir;
    dc.prebuffer_segments   = cfg.prebuffer_segments;
    dc.buffer_minutes       = cfg.buffer_minutes;
    dc.max_cached_segments  = cfg.max_cached_segments;
    dc.keep_behind_segments = cfg.keep_behind_segments;
    dc.stale_after_ms       = cfg.stale_after_ms;
    dc.pinned_event_id      = cfg.pinned_event_id;
    m_session = std::make_shared<DecoderSession>(dc, *m_transport);

    CatalogConfig cc;
    cc.room_id        = cfg.room_id;
    cc.stale_after_ms = cfg.stale_after_ms;
    m_catalog = std::make_shared<EventCatalog>(cc, *m_transport);

    plog_info("receiving room '%s' from %s", cfg.room_id.c_str(),
              m_transport->base_url().c_str());
    note_error("");
}

void Player::start() {
    if (m_running.exchange(true)) return;

    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        rebuild_session();
    }

    m_poll_thread    = std::thread([this] { poll_loop(); });
    m_feed_thread    = std::thread([this] { feed_loop(); });
    m_deliver_thread = std::thread([this] { deliver_loop(); });

    m_events_wanted = true;

    // An appliance that needs somebody to press Play after a power cut is not
    // an appliance.
    if (config().auto_play) {
        m_playing = true;
        plog_info("auto-play is on — going to air as soon as there is a feed");
    }
}

void Player::stop() {
    m_flushing = true;                 // release a decoder blocked on the queue
    if (!m_running.exchange(false)) { m_flushing = false; return; }
    m_dq_cv.notify_all();
    if (m_poll_thread.joinable())    m_poll_thread.join();
    if (m_feed_thread.joinable())    m_feed_thread.join();
    if (m_deliver_thread.joinable()) m_deliver_thread.join();
    m_flushing = false;

    teardown_decoder();
    {
        std::lock_guard<std::mutex> lk(m_dq_mtx);
        m_dq.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        m_session.reset();
        m_catalog.reset();
        m_transport.reset();
    }
    // Never leave the last frame of a service on a screen in an empty room.
    m_video.blank();
    m_audio.close();
    m_audio_open = false;
}

void Player::teardown_decoder() {
    std::shared_ptr<CmafDecoder> old;
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        old = m_decoder;
        m_decoder.reset();
    }
    // stop() joins the decode thread and can block; never under the lock.
    if (old) old->stop();
    m_decoder_started = false;
    m_first_pts_ns = -1;
    m_logged_av_offset = false;
    m_last_video_pts_ns = 0;
}

void Player::flush_delivery() {
    m_flushing = true;
    {
        std::lock_guard<std::mutex> lk(m_dq_mtx);
        m_dq.clear();
    }
    m_dq_cv.notify_all();
    m_flushing = false;
    m_first_pts_ns = -1;               // the next frame re-anchors the clock
    m_audio.flush();
    m_dq_cv.notify_all();
}

void Player::reconfigure(const Config& cfg) {
    Config before = config();
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg = cfg;
    }
    m_locked = cfg.locked;
    m_delay_from_live_s = cfg.delay_from_live_s;

    // Only a change to what is being received justifies taking the picture
    // away. Editing the idle colour must not interrupt a service.
    const bool receive_changed =
        before.endpoint_host      != cfg.endpoint_host ||
        before.r2_account_id      != cfg.r2_account_id ||
        before.bucket             != cfg.bucket ||
        before.access_key_id      != cfg.access_key_id ||
        before.secret_access_key  != cfg.secret_access_key ||
        before.region             != cfg.region ||
        before.room_id            != cfg.room_id ||
        before.cache_dir          != cfg.cache_dir ||
        before.prebuffer_segments != cfg.prebuffer_segments ||
        before.buffer_minutes     != cfg.buffer_minutes ||
        before.keep_behind_segments != cfg.keep_behind_segments ||
        before.max_cached_segments  != cfg.max_cached_segments ||
        before.stale_after_ms     != cfg.stale_after_ms;

    // Settings that decide where picture and sound come OUT are applied by
    // reopening those outputs, not by restarting the receive path. Changing
    // the resolution must not tear down a service's buffer, and a setting
    // that only takes effect after a reboot is no use on a box with no
    // keyboard.
    const bool display_changed =
        before.drm_card   != cfg.drm_card   ||
        before.connector  != cfg.connector  ||
        before.out_width  != cfg.out_width  ||
        before.out_height != cfg.out_height ||
        before.out_fps    != cfg.out_fps;

    if (display_changed) {
        plog_info("display settings changed — resetting the output");
        m_video.close();
        std::string err;
        if (!m_video.open(cfg, err)) {
            plog_error("display: %s", err.c_str());
            note_error("display: " + err);
        } else {
            plog_info("display: %s", m_video.description().c_str());
        }
        // Whatever was on the screen belongs to the old mode.
        m_idle_showing = false;
        m_last_frame_ns = 0;
    }

    if (before.alsa_device    != cfg.alsa_device ||
        before.audio_channels != cfg.audio_channels ||
        before.audio_enabled  != cfg.audio_enabled) {
        plog_info("sound settings changed — reopening the output");
        m_audio_reopen = true;
        if (!cfg.audio_enabled) { m_audio.close(); m_audio_open = false; }
    }

    if (!receive_changed) {
        plog_info("settings saved");
        return;
    }

    plog_info("storage settings changed — reconnecting");
    teardown_decoder();
    flush_delivery();
    {
        std::lock_guard<std::mutex> lk(m_obj_mtx);
        rebuild_session();
    }
    m_events_wanted = true;
    m_poll_now = true;
}

// ── Frame arrival (decode thread) ────────────────────────────────────────────

int64_t Player::anchor_pts(int64_t pts_ns, bool is_video) {
    int64_t first = m_first_pts_ns.load();
    if (first < 0) {
        m_first_pts_ns = pts_ns;
        m_playout_base_ns = now_ns() + kPlayoutCushionNs;
        first = pts_ns;
        plog_debug("playout anchored on first %s frame (pts %.3fs)",
                   is_video ? "video" : "audio", (double)pts_ns / 1e9);
    }
    return first;
}

void Player::enqueue(PendingFrame&& f) {
    std::unique_lock<std::mutex> lk(m_dq_mtx);
    // Bounded wait. Waiting indefinitely for space would let the decoder's
    // thread block inside this callback whenever delivery stopped draining,
    // and CmafDecoder::stop() would then join a thread that could never
    // finish. Dropping a frame is far better than hanging the appliance.
    const bool space = m_dq_cv.wait_for(lk, std::chrono::milliseconds(250),
        [this] {
            return m_dq.size() < kMaxQueuedFrames || !m_running.load() ||
                   m_flushing.load();
        });
    if (!m_running.load() || m_flushing.load()) return;
    if (!space) { m_frames_dropped++; return; }
    m_dq.push_back(std::move(f));
    lk.unlock();
    m_dq_cv.notify_all();
}

void Player::on_video(const DecodedVideoFrame& f) {
    if (!m_running.load()) return;
    const int64_t first = anchor_pts(f.pts_ns, true);
    m_last_video_pts_ns = f.pts_ns;

    PendingFrame item;
    item.is_video = true;
    item.due_ns   = m_playout_base_ns.load() + (uint64_t)(f.pts_ns - first);
    item.video    = f;                 // deep copy; owns its planes
    fix_planes(item.video);
    enqueue(std::move(item));
}

void Player::on_audio(const DecodedAudioFrame& f) {
    if (!m_running.load()) return;
    const int64_t first = anchor_pts(f.pts_ns, false);

    if (!m_logged_av_offset && m_last_video_pts_ns != 0) {
        m_logged_av_offset = true;
        plog_debug("audio/video pts offset %.3fs (should be near zero)",
                   (double)(f.pts_ns - m_last_video_pts_ns) / 1e9);
    }

    PendingFrame item;
    item.is_video = false;
    item.due_ns   = m_playout_base_ns.load() + (uint64_t)(f.pts_ns - first);
    item.audio    = f;
    enqueue(std::move(item));
}

// ── Delivery ─────────────────────────────────────────────────────────────────

void Player::deliver_loop() {
    plog_info("delivery started");

    while (m_running.load()) {
        // While held, deliver nothing: the picture stays on the last frame the
        // output received and the queue stays put, so Continue resumes exactly
        // where the operator stopped.
        if (m_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        PendingFrame item;
        {
            std::unique_lock<std::mutex> lk(m_dq_mtx);
            m_dq_cv.wait_for(lk, std::chrono::milliseconds(50), [this] {
                return !m_dq.empty() || !m_running.load();
            });
            if (!m_running.load()) break;
            if (m_dq.empty()) continue;
            // Release the earliest-timestamped frame in the window, not simply
            // the first enqueued: video and audio arrive in track order.
            auto it = std::min_element(m_dq.begin(), m_dq.end(),
                [](const PendingFrame& a, const PendingFrame& b) {
                    return a.due_ns < b.due_ns;
                });
            item = std::move(*it);
            m_dq.erase(it);
        }
        m_dq_cv.notify_all();

        // Far past due means the clock drifted behind wall time, normally
        // because playback stalled waiting for a segment. Re-anchor by
        // ASSIGNING, never accumulating: a second resync for the same stall
        // must be a no-op, or the clock jumps twice and playback stalls for
        // good.
        {
            const uint64_t now = now_ns();
            if (item.due_ns + kClockResyncThresholdNs < now) {
                const int64_t first = m_first_pts_ns.load();
                const int64_t pts = item.is_video ? item.video.pts_ns
                                                  : item.audio.pts_ns;
                const uint64_t behind = now - item.due_ns;
                const uint64_t base = now - (uint64_t)(pts - first)
                                    + kMaxDeliveryLeadNs;
                m_playout_base_ns = base;
                item.due_ns = base + (uint64_t)(pts - first);
                {
                    std::lock_guard<std::mutex> lk(m_dq_mtx);
                    for (auto& q : m_dq) {
                        const int64_t qp = q.is_video ? q.video.pts_ns
                                                      : q.audio.pts_ns;
                        q.due_ns = base + (uint64_t)(qp - first);
                    }
                }
                const uint64_t last = m_last_resync_log_ns.load();
                if (now - last > 5000000000ULL) {
                    m_last_resync_log_ns = now;
                    plog_warn("playout fell %.1fs behind (waiting for the "
                              "feed?) — re-anchored", (double)behind / 1e9);
                }
            }
        }

        // Hold until due, in slices so shutdown stays responsive.
        while (m_running.load()) {
            const uint64_t now = now_ns();
            if (item.due_ns <= now + kMaxDeliveryLeadNs) break;
            uint64_t wait = item.due_ns - now - kMaxDeliveryLeadNs;
            if (wait > 20000000ULL) wait = 20000000ULL;
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
        }
        if (!m_running.load()) break;

        // Sub-segment seek: drop frames before the requested moment. Segments
        // are the unit of transfer; they need not be the unit of seeking.
        {
            const int64_t skip = m_skip_until_pts_ns.load();
            if (skip >= 0) {
                int64_t base = m_seg_first_pts_ns.load();
                const int64_t pts = item.is_video ? item.video.pts_ns
                                                  : item.audio.pts_ns;
                if (base < 0) { m_seg_first_pts_ns = pts; base = pts; }
                if (pts - base < skip) continue;
                m_skip_until_pts_ns = -1;
            }
        }

        // Keep the reported time in step with the frame going to air, so it
        // advances continuously rather than once per segment.
        {
            int64_t base = m_seg_first_pts_ns.load();
            const int64_t pts = item.is_video ? item.video.pts_ns
                                              : item.audio.pts_ns;
            if (base < 0) { m_seg_first_pts_ns = pts; base = pts; }
            const long long segstart = m_seg_starts_at_ms.load();
            if (segstart > 0)
                m_playing_at_ms = segstart + (pts - base) / 1000000LL;
        }

        // Something has reached the output, so whatever was asked for has
        // landed. Keyed on delivery rather than on the seek returning: the
        // seek is instant, arriving there is not.
        if (m_awaiting_frames.exchange(false)) {
            m_seek_target_ms = 0;
            m_loading_event  = false;
        }

        if (item.is_video) {
            m_video.present(item.video);
            m_frames_out++;
            m_last_frame_ns = now_ns();
            m_idle_showing = false;
            // Keep the newest picture for the preview. Copied under its own
            // lock so a browser reading it can never stall the output.
            {
                std::lock_guard<std::mutex> lk(m_frame_mtx);
                m_last_frame = item.video;
                fix_planes(m_last_frame);
            }
            m_frame_version++;
        } else {
            Config cfg = config();
            // The device was changed in the interface. Let the old one go so
            // the next frame opens the new one.
            if (m_audio_reopen.exchange(false)) {
                m_audio.close();
                m_audio_open = false;
            }
            if (cfg.audio_enabled && !m_audio_open.load()) {
                const int ch = cfg.audio_channels > 0 ? cfg.audio_channels
                                                      : item.audio.channels;
                std::string err;
                if (m_audio.open(cfg, item.audio.sample_rate, ch, err)) {
                    m_audio_open = true;
                    plog_info("audio out: %s", m_audio.description().c_str());
                } else {
                    plog_error("audio out failed: %s", err.c_str());
                    note_error("audio output: " + err);
                    // Do not retry every frame; a dead card would fill the log
                    // faster than an operator could read it. Changing the
                    // device in the interface asks for another attempt.
                    m_audio_open = true;
                }
            }
            if (cfg.audio_enabled) m_audio.write(item.audio);
        }
    }
    plog_info("delivery stopped");
}

// ── The idle screen ──────────────────────────────────────────────────────────

void Player::update_screen() {
    // Frames are reaching the display: there is a service on, and nothing
    // here should touch the screen.
    const uint64_t last = m_last_frame_ns.load();
    if (last != 0 && now_ns() - last < 2000000000ULL) {
        m_idle_showing = false;
        return;
    }

    const Config cfg = config();

    // Holding the last picture is the pause behaviour, so there is by
    // definition nothing to draw: leave the screen exactly as it is.
    if (cfg.idle_mode == IdleMode::HoldFrame && m_frames_out.load() > 0) return;

    if (cfg.idle_mode == IdleMode::Black) {
        if (!m_idle_showing) { m_video.blank(); m_idle_showing = true; }
        return;
    }

    int width = 1920, height = 1080;
    m_video.size(width, height);

    if (cfg.idle_mode == IdleMode::Image) {
        if (m_idle_showing) return;      // a still does not change
        Canvas canvas(width, height);
        std::string err;
        if (load_still(cfg.idle_image_path, width, height, canvas, err)) {
            m_video.present_bgrx(canvas.width(), canvas.height(),
                                 canvas.stride(), canvas.pixels());
            m_idle_showing = true;
            return;
        }
        // A holding slide that cannot be read must not leave a black screen
        // with no explanation — fall through to the splash, which at least
        // says where the box is.
        plog_warn("holding slide: %s", err.c_str());
    }

    SplashInfo info;
    info.hostname   = hostname();
    info.room       = cfg.room_id;
    info.version    = player_version();
    info.configured = cfg.configured();
    for (const auto& n : network_interfaces()) {
        if (n.ipv4.empty()) continue;
        info.addresses.push_back("HTTP://" + n.ipv4 + ":" +
                                 std::to_string(cfg.web_port));
    }

    // The state line, in the words an operator would use standing in front of
    // the screen.
    if (!cfg.configured()) {
        // The splash already says the box has no storage details; repeating
        // it as a state line would just be the same sentence twice.
        info.state.clear();
    } else if (auto sess = session_ref()) {
        switch (sess->room_state()) {
        case RoomState::Live:
            info.state = m_playing.load() ? "STARTING" : "READY - NOT ON AIR";
            break;
        case RoomState::Ended:
            info.state = "RECORDING READY";
            break;
        case RoomState::Interrupted:
            info.state = "LAST SERVICE WAS CUT SHORT";
            break;
        case RoomState::Offline:
            info.state = "WAITING FOR THE MAIN SITE";
            break;
        default:
            info.state = "LOOKING FOR THE MAIN SITE";
            break;
        }
        const double ahead = sess->buffered_ahead_s();
        if (ahead > 1) {
            info.detail = std::to_string((int)(ahead / 60)) +
                          " MINUTES READY TO PLAY";
        }
    } else {
        info.state = "WAITING FOR THE MAIN SITE";
    }

    // Nothing has changed, so nothing needs redrawing.
    std::string signature = info.state + "|" + info.detail + "|" + info.room;
    for (const auto& a : info.addresses) signature += "|" + a;
    if (m_idle_showing && signature == m_idle_signature) return;
    m_idle_signature = signature;

    Canvas canvas(width, height);
    render_splash(canvas, info);
    m_video.present_bgrx(canvas.width(), canvas.height(), canvas.stride(),
                         canvas.pixels());
    m_idle_showing = true;
}

// ── Poll loop ────────────────────────────────────────────────────────────────

void Player::poll_loop() {
    plog_info("receive loop started");
    long long next_poll = 0;
    long long last_status_log = 0;

    while (m_running.load()) {
        const long long now = now_ms();
        const int interval = config().poll_interval_ms;

        if (now >= next_poll || m_poll_now.exchange(false)) {
            next_poll = now + interval;
            auto sess = session_ref();
            if (sess) {
                // poll() does network I/O and can take seconds; never under a
                // lock.
                const RoomState st = sess->poll();

                // Load does not go to air, so no frame will arrive to clear
                // this — the poll that performed the switch has to.
                if (m_loading_event.load() && st != RoomState::Unknown)
                    m_loading_event = false;

                if ((int)st != m_last_room) {
                    m_last_room = (int)st;
                    const char* name =
                        st == RoomState::Live  ? "LIVE"
                      : st == RoomState::Ended ? (sess->was_live_this_session()
                            ? "BROADCAST ENDED — playing out the recording"
                            : "a finished recording (was not live when loaded)")
                      : st == RoomState::Interrupted
                            ? "INTERRUPTED — the encoder stopped without "
                              "ending; playing what was recorded"
                      : st == RoomState::Offline ? "offline" : "unknown";
                    plog_info("room is %s", name);
                    if (!sess->last_error().empty())
                        note_error(sess->last_error());
                }
            }
        }

        // Download-ahead runs continuously — this is what keeps filling the
        // cache while the picture is held or sitting behind live.
        int fetched = 0;
        if (auto sess = session_ref()) fetched = sess->pump_downloads(8);

        // The event list, on this thread rather than a browser's. A listing
        // plus one manifest per event is seconds of network work.
        if (m_events_wanted.exchange(false)) {
            std::shared_ptr<EventCatalog> cat;
            { std::lock_guard<std::mutex> lk(m_obj_mtx); cat = m_catalog; }
            if (cat) {
                m_events_refreshing = true;
                cat->refresh();
                EventListing listing;
                listing.listed_once   = true;
                listing.fallback_scan = cat->used_fallback_scan();
                listing.skipped       = cat->skipped();
                listing.error         = cat->last_error();
                for (const auto& e : cat->events()) {
                    EventEntry row;
                    row.event_id   = e.event_id;
                    row.started_ms = (long long)e.started_at_ms;
                    row.duration_s = e.duration_s;
                    row.state      = (int)e.state;
                    listing.events.push_back(std::move(row));
                }
                const size_t count = listing.events.size();
                {
                    std::lock_guard<std::mutex> lk(m_events_mtx);
                    m_events = std::move(listing);
                }
                m_events_refreshing = false;
                plog_info("event list refreshed — %zu event(s)%s", count,
                          cat->used_fallback_scan()
                              ? " (scanned events/: these predate the room index)"
                              : "");
            }
        }

        update_screen();

        if (now - last_status_log > 60000) {
            last_status_log = now;
            if (auto sess = session_ref()) {
                const auto& s = sess->stats();
                plog_info("head=%llu live=%llu behind=%.0fs buffered=%.0fs "
                          "cached=%zu downloaded=%llu frames_out=%llu",
                          (unsigned long long)sess->playback_head(),
                          (unsigned long long)sess->live_edge(),
                          sess->behind_live_s(), sess->buffered_ahead_s(),
                          sess->cache().count(),
                          (unsigned long long)s.downloaded,
                          (unsigned long long)m_frames_out.load());
            }
        }

        // Always yield, even when there is more to fetch: a tight download
        // loop starves everything else, and a few milliseconds costs nothing
        // against a segment download.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(fetched == 0 ? 100 : 5));
    }
    plog_info("receive loop stopped");
}

// ── Feed loop ────────────────────────────────────────────────────────────────

void Player::feed_loop() {
    plog_info("feed loop started");
    while (m_running.load()) {
        // While held, stop pulling and feeding entirely: otherwise the queues
        // fill, push_fragment blocks, and Continue cannot get in.
        if (m_paused.load() || !m_playing.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto sess = session_ref();
        if (!sess) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (sess->play_state() == PlayState::Stopped) sess->start();
        std::optional<PlayableSegment> seg = sess->next_segment();
        if (!seg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // A jump means the next fragment belongs to a different timeline.
        // Feeding across one produces out-of-order timestamps and a glitched
        // picture, so the decoder is torn down and restarts from the re-sent
        // init segment.
        {
            const uint64_t d = sess->discontinuity_id();
            if (d != m_seen_discontinuity) {
                m_seen_discontinuity = d;
                if (m_decoder_started.load()) {
                    plog_info("playback jumped — restarting the decoder");
                    teardown_decoder();
                    {
                        std::lock_guard<std::mutex> lk(m_dq_mtx);
                        m_dq.clear();
                    }
                    m_dq_cv.notify_all();
                }
            }
        }

        if (!m_decoder_started.load()) {
            if (seg->init.empty()) {
                plog_error("first segment arrived with no init segment — "
                           "cannot start decoding");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            auto dec = std::make_shared<CmafDecoder>();
            dec->on_video([this](const DecodedVideoFrame& f) { on_video(f); });
            dec->on_audio([this](const DecodedAudioFrame& f) { on_audio(f); });
            if (!dec->start(seg->init)) {
                plog_error("decoder failed to start: %s", dec->error().c_str());
                note_error("decoder: " + dec->error());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            { std::lock_guard<std::mutex> lk(m_obj_mtx); m_decoder = dec; }
            m_decoder_started = true;
            m_feed_start_ns = now_ns();
            m_pushed_media_ns = 0;
            plog_info("decoder started (%dx%d, %d audio track(s))",
                      dec->video_width(), dec->video_height(),
                      dec->audio_track_count());
        }

        // Feed at playout rate with a small lead, so the decoder always has
        // work but never runs seconds ahead of the clock.
        while (m_running.load() && !m_paused.load()) {
            const uint64_t elapsed = now_ns() - m_feed_start_ns;
            if (m_pushed_media_ns <= elapsed + kFeedLeadNs) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!m_running.load()) break;

        m_seg_starts_at_ms = (long long)seg->starts_at_ms;
        m_seg_first_pts_ns = -1;                 // set by the first frame
        if (seg->skip_to_ms > 0)
            m_skip_until_pts_ns = seg->skip_to_ms * 1000000LL;

        // push_fragment blocks when the decoder is full — never under a lock.
        if (auto dec = decoder_ref()) dec->push_fragment(seg->media);
        m_pushed_media_ns += (uint64_t)(seg->duration_s * 1e9);
    }
    plog_info("feed loop stopped");
}

// ── Operator controls ────────────────────────────────────────────────────────

void Player::play() {
    auto sess = session_ref();
    if (!sess) { plog_warn("play ignored — no storage configured"); return; }
    m_paused = false;
    m_playing = true;
    sess->resume();
    plog_info("PLAY — %.0fs behind live, %.0fs buffered",
              sess->behind_live_s(), sess->buffered_ahead_s());
}

void Player::stop_playback() {
    m_playing = false;
    m_paused = false;
    flush_delivery();
    // Stopping is a deliberate act, so the screen should reflect it rather
    // than keeping the last frame of a service up. Downloading continues.
    Config cfg = config();
    if (cfg.idle_mode != IdleMode::HoldFrame) m_video.blank();
    plog_info("STOPPED (still downloading, ready to play again)");
}

void Player::pause() {
    auto sess = session_ref();
    if (!sess) return;
    // Order matters: stop delivery first so the picture holds immediately,
    // then stop pulling new segments.
    m_paused = true;
    sess->pause();
    plog_info("HOLDING the picture — the cache keeps filling");
}

void Player::resume() {
    auto sess = session_ref();
    if (!sess) { plog_warn("continue ignored — no session"); return; }
    // Re-anchor rather than shift. Advancing the clock by the paused duration
    // has several ways to fail quietly, and every one of them leaves frames
    // that never become due — a picture frozen for good. Treating it as a
    // discontinuity costs the handful of frames still queued and always works.
    flush_delivery();
    m_paused = false;
    sess->resume();
    plog_info("CONTINUING at %.0fs behind live", sess->behind_live_s());
}

void Player::toggle_pause() {
    if (m_paused.load()) resume(); else pause();
}

void Player::jump_to_live() {
    auto sess = session_ref();
    if (!sess) return;
    sess->jump_to_live();
    m_paused = false;
    m_delay_from_live_s = 0.0;
    plog_info("CAUGHT UP TO NOW");
}

void Player::seek_to_time(long long wall_ms) {
    auto sess = session_ref();
    if (!sess) return;
    const int64_t got = sess->seek_to_wall_ms((int64_t)wall_ms);
    if (got == 0) {
        plog_warn("that moment is no longer available in storage");
        note_error("that moment is no longer available in storage");
        return;
    }
    flush_delivery();
    m_paused = false;

    // Report where playback is GOING immediately. Waiting until a frame has
    // been fetched and decoded there made a jog look like a dropped click that
    // then snapped into place; the UI marks this as provisional until frames
    // arrive.
    m_seek_target_ms  = (long long)got;
    m_playing_at_ms   = (long long)got;
    m_awaiting_frames = true;
    m_poll_now        = true;
    plog_info("went to %lld (%.0fs behind live)", (long long)got,
              sess->behind_live_s());
}

void Player::jog(double seconds) {
    long long from = m_playing_at_ms.load();
    if (from <= 0) {
        // Nothing delivered yet — a recording loaded but not played, which is
        // exactly when an operator wants to move to their intended start
        // point. Jog from where the playhead SITS rather than refusing.
        if (auto sess = session_ref()) from = (long long)sess->playhead_wall_ms();
    }
    if (from <= 0) {
        plog_warn("cannot jog until the recording has loaded");
        return;
    }
    seek_to_time(from + (long long)(seconds * 1000.0));
}

void Player::set_delay_from_live(double seconds) {
    auto sess = session_ref();
    if (!sess) return;
    const int64_t live = sess->live_wall_ms();
    if (live <= 0) {
        plog_warn("the live time is not known yet");
        return;
    }
    m_delay_from_live_s = seconds;
    seek_to_time((long long)live - (long long)(seconds * 1000.0));
    plog_info("holding %.0f minute(s) behind live", seconds / 60.0);
}

void Player::jump_to_marker(const std::string& id) {
    auto sess = session_ref();
    if (!sess) return;
    if (!sess->jump_to_marker(id)) {
        plog_warn("could not jump to that cue — it may no longer be retained");
        note_error("that cue is no longer available");
        return;
    }
    flush_delivery();
    m_paused = false;
    m_awaiting_frames = true;
    m_poll_now = true;
    plog_info("jumped to a cue");
}

void Player::pin_event(const std::string& event_id) {
    auto sess = session_ref();
    if (!sess) return;
    sess->pin_event(event_id);
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg.pinned_event_id = event_id;
    }
    teardown_decoder();
    flush_delivery();
    m_loading_event = true;
    m_poll_now = true;
    plog_info("loading event %s", event_id.c_str());
}

void Player::unpin_event() {
    auto sess = session_ref();
    if (!sess) return;
    sess->unpin();
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg.pinned_event_id.clear();
    }
    teardown_decoder();
    flush_delivery();
    m_loading_event = true;
    m_poll_now = true;
    m_events_wanted = true;
    plog_info("following whatever is live in the room");
}

void Player::set_locked(bool locked) {
    m_locked = locked;
    {
        std::lock_guard<std::mutex> lk(m_cfg_mtx);
        m_cfg.locked = locked;
    }
    plog_info(locked ? "controls locked" : "controls unlocked");
}

void Player::refresh_events() { m_events_wanted = true; }

void Player::event_listing(EventListing& out) const {
    {
        std::lock_guard<std::mutex> lk(m_events_mtx);
        out = m_events;
    }
    out.loading = m_events_refreshing.load();
}

bool Player::latest_frame(DecodedVideoFrame& out, uint64_t& version) const {
    std::lock_guard<std::mutex> lk(m_frame_mtx);
    if (m_last_frame.data.empty()) return false;
    out = m_last_frame;
    fix_planes(out);
    version = m_frame_version.load();
    return true;
}

// ── Status ───────────────────────────────────────────────────────────────────

void Player::status(Status& out) const {
    out = Status{};
    Config cfg = config();

    out.room_id           = cfg.room_id;
    out.configured        = cfg.configured();
    out.locked            = m_locked.load();
    out.playing           = m_playing.load();
    out.paused            = m_paused.load();
    out.loading           = m_loading_event.load();
    out.seek_target_ms    = m_seek_target_ms.load();
    out.delay_from_live_s = m_delay_from_live_s.load();
    out.frames_out        = m_frames_out.load();
    out.frames_dropped    = m_frames_dropped.load();

    out.output_description = m_video.description();
    out.video_output_ok    = m_video.ok();
    out.audio_description  = cfg.audio_enabled ? m_audio.description()
                                               : std::string("muted");
    out.audio_output_ok    = !cfg.audio_enabled || m_audio.ok();

    {
        std::lock_guard<std::mutex> lk(m_err_mtx);
        out.last_error = m_last_error;
    }

    auto sess = session_ref();
    if (!sess) return;

    out.room_state     = (int)sess->room_state();
    out.event_id       = sess->event_id();
    out.pinned_event_id = sess->pinned_event();
    out.live_elsewhere = sess->live_elsewhere();
    out.live_event_id  = sess->live_event_id();

    out.ended       = sess->event_ended();
    out.at_end      = sess->at_end();
    out.was_live    = sess->was_live_this_session();
    out.interrupted = sess->was_interrupted();

    out.live_ms     = sess->live_wall_ms();
    out.earliest_ms = sess->earliest_wall_ms();
    out.started_ms  = sess->event_started_ms();
    out.end_ms      = sess->end_wall_ms();
    if (out.ended && out.end_ms > 0 && out.started_ms > 0)
        out.total_ms = out.end_ms - out.started_ms;

    // The delivered time is the honest one — it is what is actually on the
    // screen. Fall back to the playhead before anything has gone out.
    const long long playing_at = m_playing_at_ms.load();
    out.playhead_ms = playing_at > 0 ? playing_at
                                     : (long long)sess->playhead_wall_ms();
    // Never report past the end of a recording: once playback runs past the
    // last segment the playhead points at a position that does not exist.
    if (out.ended && out.end_ms > 0 && out.playhead_ms > out.end_ms)
        out.playhead_ms = out.end_ms;

    out.behind_live_s    = sess->behind_live_s();
    out.buffered_ahead_s = sess->buffered_ahead_s();
    out.cached_segments  = sess->cache().count();
    out.buffering        = m_playing.load() && !m_paused.load() &&
                           m_frames_out.load() == 0;

    const auto& st = sess->stats();
    out.downloaded        = st.downloaded;
    out.download_failures = st.download_failures;
    out.checksum_failures = st.checksum_failures;
    out.gaps_waited       = st.gaps_waited;

    if (out.last_error.empty()) out.last_error = sess->last_error();

    for (const auto& span : sess->cached_ranges()) {
        const int64_t a = sess->wall_clock_ms(span.first);
        const int64_t b = sess->wall_clock_ms(span.second);
        if (a > 0 && b >= a) out.cached_spans.emplace_back((long long)a,
                                                           (long long)b);
    }

    for (const auto& m : sess->markers()) {
        Status::MarkerEntry e;
        e.label = m.label;
        e.id    = m.id;
        e.at_ms = (long long)sess->wall_clock_ms(m.seq);
        out.markers.push_back(std::move(e));
    }
    if (auto cur = sess->current_marker()) out.current_marker = cur->label;

    for (const auto& t : sess->audio_layout()) {
        out.audio_channels = std::max(out.audio_channels, t.channels);
        for (const auto& lbl : t.channel_labels)
            out.channel_labels.push_back(lbl);
        if (t.channel_labels.empty() && !t.label.empty())
            out.channel_labels.push_back(t.label);
    }

    if (auto dec = decoder_ref()) {
        out.video_width  = dec->video_width();
        out.video_height = dec->video_height();
    }
}

} // namespace multisite_player
