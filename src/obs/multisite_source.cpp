// multisite_source.cpp — the satellite (receive) side, as an OBS source.
//
// Threads:
//   poll_loop     — refreshes live.json/manifest.json, drives download-ahead
//   feed_loop     — hands cached fragments to the decoder in order
//   decode        — inside CmafDecoder (its own thread), emits frames
// video_tick does nothing heavy; frames are pushed to OBS from the decoder
// callbacks with timestamps on OBS's clock, so OBS's async buffering paces
// playout.
//
// Timeslipping controls (Pause / Resume / Jump to live) live in the source
// properties for now; the Qt dock comes in Phase 5.
//
#include <obs-module.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <util/platform.h>

#include "plugin_log.h"
#include "multisite_ui.h"
#include "decoder_settings.h"

#include "../core/decoder_session.h"
#include "../core/cmaf_decoder.h"
#include "../core/s3_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_obs {

using namespace multisite;

// setting keys
static constexpr char S_ENDPOINT[] = "endpoint_host";
static constexpr char S_ACCOUNT[]  = "r2_account_id";
static constexpr char S_BUCKET[]   = "bucket";
static constexpr char S_KEYID[]    = "access_key_id";
static constexpr char S_SECRET[]   = "secret_access_key";
static constexpr char S_REGION[]   = "region";
static constexpr char S_ROOM[]     = "room_id";
static constexpr char S_POLL_MS[]  = "poll_interval_ms";
static constexpr char S_PREBUF[]   = "prebuffer_segments";
static constexpr char S_KEEP[]     = "keep_behind_segments";

// One entry in the delivery queue: either a video or an audio frame, already
// stamped with its OBS presentation time.
struct PendingFrame {
    bool     is_video = true;
    uint64_t timestamp = 0;
    DecodedVideoFrame video;
    DecodedAudioFrame audio;
};

// Exposes timeslipping to hotkeys and the Tools menu.
struct SourceCtx : DecoderControls {
    obs_source_t* source = nullptr;

    // Held as shared_ptr behind a short-lived lock. The worker threads and the
    // UI both need these, and some calls block (network polls, and pushing a
    // fragment when the decoder is full). Holding a mutex across a blocking
    // call deadlocked Resume against the feed loop, so callers now take a
    // reference under `obj_mtx` and release it before doing any work — the
    // session and decoder are internally thread-safe.
    std::shared_ptr<S3Transport>    transport;
    std::shared_ptr<DecoderSession> session;
    std::shared_ptr<CmafDecoder>    decoder;
    mutable std::mutex              obj_mtx;
    // Serialises src_update / src_destroy. OBS can apply settings from more
    // than one thread, and two overlapping updates could leave two sets of
    // worker threads running — which showed up as every log line appearing
    // twice and as clock adjustments being applied twice.
    std::mutex                      lifecycle_mtx;

    std::thread poll_thread, feed_thread;
    std::atomic<bool> running{false};

    // Guards session/decoder lifetime against the worker threads.
    std::mutex mtx;

    int  poll_interval_ms = 3000;
    uint32_t width = 0, height = 0;

    // Playout clock: OBS timestamps = base + (media pts - first media pts).
    std::atomic<uint64_t> playout_base_ns{0};
    std::atomic<int64_t>  first_pts_ns{-1};
    std::atomic<bool>     decoder_started{false};
    uint64_t              seen_discontinuity = 0;

    // Realtime pacing. OBS's async buffer only holds a fraction of a second,
    // so fragments must be fed at roughly the rate they play out — decoding
    // as fast as the cache allows would hand OBS frames seconds into the
    // future, which it drops (producing jumpy video).
    int64_t  last_video_pts_ns = 0;
    bool     logged_av_offset = false;
    std::atomic<bool> checked_layout{false};

    // Resume watchdog: if no frames reach OBS shortly after a resume, dump
    // enough state to explain why instead of leaving a frozen picture and no
    // clue in the log.
    std::atomic<uint64_t> resumed_at_ns{0};
    std::atomic<uint64_t> frames_at_resume{0};
    std::atomic<bool>     resume_checked{false};

    // The clock time of the frame currently on screen. Derived from the frame
    // being delivered, so it advances continuously rather than jumping once
    // per segment — which is why the displayed time appeared frozen.
    std::atomic<long long> playing_at_ms{0};
    // Wall time and first media pts of the current segment, used to convert a
    // frame's pts into a clock reading.
    std::atomic<long long> seg_starts_at_ms{0};
    std::atomic<long long> seg_first_pts_ns{-1};
    // After a timed seek, frames earlier than this point in the segment are
    // dropped, giving roughly one-second accuracy instead of six.
    std::atomic<long long> skip_until_pts_ns{-1};

    // An operator loads an event, lets it buffer, then presses Play on cue.
    // Auto-playing as soon as enough is buffered is wrong for a service.
    std::atomic<bool> playing{false};
    // Guards against accidental clicks mid-service.
    std::atomic<bool> controls_locked{false};
    // Set while the queue is being torn down (seek, stop, decoder restart) so
    // the decoder's callbacks return immediately instead of waiting for space
    // that will never come.
    std::atomic<bool> flushing{false};
    std::atomic<uint64_t> frames_dropped{0};
    std::atomic<uint64_t> last_resync_log_ns{0};

    // DecoderControls — driven by hotkeys and the Tools menu.
    void pause() override;
    void resume() override;
    void toggle_pause() override;
    void jump_to_live() override;
    void log_status() override;
    void snapshot(DecoderSnapshot& out) const override;
    void jump_to_marker(const std::string& id) override;
    void seek(unsigned long long seq) override;
    void reconfigure() override;
    void play() override;
    void stop_playback() override;
    bool is_playing() const override { return playing.load(); }
    void seek_to_time(long long wall_ms) override;
    void jog(double seconds) override;
    void set_delay_from_live(double seconds) override;
    void set_locked(bool l) override { controls_locked = l; }
    bool locked() const override { return controls_locked.load(); }

    // Marker chosen in the properties dialog, acted on by the Jump button.
    std::string pending_marker_id;
    std::string room_id_for_display;
    std::atomic<int> audio_channels{0};
    // Wall clock when playback was paused, so the playout clock can be
    // advanced by the same amount on resume (see on_resume).
    std::atomic<uint64_t> pause_started_ns{0};
    // Pause is enforced at DELIVERY, not at segment feeding: by the time a
    // fragment is fed, its whole 6 s is already decoded, so gating the feed
    // would let playback run on for up to a segment after the click. Holding
    // frames here makes pause and resume take effect immediately, and nothing
    // is discarded — the queue is simply not drained while paused.
    std::atomic<bool> paused{false};
    uint64_t feed_start_ns = 0;      // wall clock when this decoder started
    uint64_t pushed_media_ns = 0;    // media duration handed over so far

    // Ordered delivery queue (see the note above kMaxQueuedFrames).
    std::deque<PendingFrame> dq;
    std::mutex               dq_mtx;
    std::condition_variable  dq_cv;
    std::thread              deliver_thread;

    // status for logging
    std::atomic<uint64_t> frames_out{0};
    RoomState last_room = RoomState::Unknown;
    int64_t   last_status_log_ms = 0;
};

// ── Frame delivery ───────────────────────────────────────────────────────────
// OBS's async buffer only holds a fraction of a second (audio buffering
// defaults to ~960 ms), while decoding a 6 s fragment yields 6 s of frames
// almost instantly. So delivery has to be paced — but pacing must NOT happen
// inside the decoder callbacks: video and audio share the decode thread, so
// sleeping on a video frame delays every audio frame decoded after it, which
// makes audio arrive seconds late and OBS reset it (audible as bursts).
//
// Instead the decoder pushes frames into one queue, in decode (timestamp)
// order, and a dedicated thread releases them when they are nearly due. Audio
// and video therefore stay together and neither starves the other.
static constexpr uint64_t kMaxDeliveryLeadNs = 400000000ULL;   // 400 ms
// The decoder now emits frames in presentation order (it sorts each
// fragment's packets by timestamp before decoding), so this queue only needs
// to smooth small jitter rather than absorb a track-ordering skew. Measured
// against real captured segments, a window of 8 already gives zero audio
// lateness; 16 leaves margin. At 720p an I420 frame is ~1.3 MB, so this caps
// out around 21 MB instead of 60 MB.
static constexpr size_t   kMaxQueuedFrames = 16;
// If frames fall further behind wall time than this, the playout clock is
// re-anchored rather than dumping a backlog into OBS.
static constexpr uint64_t kClockResyncThresholdNs = 2000000000ULL;   // 2 s
// Take a reference under the short lock; never call through it while holding
// `obj_mtx`. This is what keeps blocking work (network polls, pushing a
// fragment to a full decoder) off the critical section that Pause/Resume need.
static std::shared_ptr<DecoderSession> get_session(SourceCtx* ctx) {
    std::lock_guard<std::mutex> lk(ctx->obj_mtx);
    return ctx->session;
}
static std::shared_ptr<CmafDecoder> get_decoder(SourceCtx* ctx) {
    std::lock_guard<std::mutex> lk(ctx->obj_mtx);
    return ctx->decoder;
}

// Push a stamped frame for delivery. Blocks while the queue is full, which
// back-pressures the decoder rather than letting memory grow.
static void enqueue_frame(SourceCtx* ctx, PendingFrame&& item) {
    std::unique_lock<std::mutex> lk(ctx->dq_mtx);
    // Bounded wait. This used to wait indefinitely for space, which meant the
    // decoder's worker thread could block inside this callback whenever
    // delivery stopped draining — after Stop, or while a seek tore the decoder
    // down. CmafDecoder::stop() then joined a thread that could never finish,
    // and because stop runs on the UI thread, OBS froze solid.
    //
    // Dropping a frame is vastly preferable to hanging the application: the
    // decoder always makes progress, so join always returns.
    const bool space = ctx->dq_cv.wait_for(
        lk, std::chrono::milliseconds(250), [ctx] {
            return ctx->dq.size() < kMaxQueuedFrames || !ctx->running.load() ||
                   ctx->flushing.load();
        });
    if (!ctx->running.load() || ctx->flushing.load()) return;
    if (!space) {
        // Delivery is not keeping up (or is stopped). Drop this frame.
        ctx->frames_dropped++;
        return;
    }
    ctx->dq.push_back(std::move(item));
    lk.unlock();
    ctx->dq_cv.notify_all();
}

// Releases frames to OBS when they are nearly due, in queue (timestamp) order,
// so audio and video are handed over together.
static void deliver_loop(SourceCtx* ctx) {
    mlog_info("source: delivery loop started");
    while (ctx->running.load()) {
        // While paused, deliver nothing: the picture holds on the last frame
        // OBS received and the queue stays put, so resume continues exactly
        // where the operator stopped.
        if (ctx->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        PendingFrame item;
        {
            std::unique_lock<std::mutex> lk(ctx->dq_mtx);
            ctx->dq_cv.wait_for(lk, std::chrono::milliseconds(50), [ctx] {
                return !ctx->dq.empty() || !ctx->running.load();
            });
            if (!ctx->running.load()) break;
            if (ctx->dq.empty()) continue;
            // Release the earliest-timestamped frame in the window, not simply
            // the first enqueued: video and audio arrive in track order, not
            // presentation order.
            auto it = std::min_element(ctx->dq.begin(), ctx->dq.end(),
                [](const PendingFrame& a, const PendingFrame& b) {
                    return a.timestamp < b.timestamp;
                });
            item = std::move(*it);
            ctx->dq.erase(it);
        }
        ctx->dq_cv.notify_all();        // let the decoder push again

        // If a frame is far past due, the playout clock has drifted behind
        // wall time — normally because playback stalled waiting for a segment
        // (a network hiccup) rather than an explicit pause. Re-anchor instead
        // of flushing a backlog at OBS, which it would report as audio lagging.
        {
            const uint64_t now = os_gettime_ns();
            if (item.timestamp + kClockResyncThresholdNs < now) {
                // Re-anchor by ASSIGNING the clock, never by accumulating on
                // to it. The previous version did `base += behind`, so if two
                // deliveries resynced for the same stall the clock jumped
                // twice — pushing every frame seconds into the future and
                // stalling playback outright (seen as frames_out frozen while
                // "behind live" kept growing). Assignment is idempotent: a
                // second resync for the same stall is a no-op.
                const long long first = ctx->first_pts_ns.load();
                const long long pts = item.is_video ? item.video.pts_ns
                                                    : item.audio.pts_ns;
                const uint64_t behind = now - item.timestamp;
                const uint64_t new_base =
                    now - (uint64_t)(pts - first) + kMaxDeliveryLeadNs;
                ctx->playout_base_ns = new_base;
                item.timestamp = new_base + (uint64_t)(pts - first);
                {
                    std::lock_guard<std::mutex> qlk(ctx->dq_mtx);
                    for (auto& q : ctx->dq) {
                        const long long qp = q.is_video ? q.video.pts_ns
                                                        : q.audio.pts_ns;
                        q.timestamp = new_base + (uint64_t)(qp - first);
                    }
                }
                // Rate-limit the message: a stall should be reported once, not
                // once per frame.
                const uint64_t last = ctx->last_resync_log_ns.load();
                if (now - last > 5000000000ULL) {
                    ctx->last_resync_log_ns = now;
                    mlog_warn("source: playout clock fell %.1fs behind "
                              "(stall?) — re-anchored", (double)behind / 1e9);
                }
            }
        }

        // Hold until nearly due, in slices so shutdown stays responsive.
        // A frame that is already due (or late) is released immediately.
        while (ctx->running.load()) {
            const uint64_t now = os_gettime_ns();
            if (item.timestamp <= now + kMaxDeliveryLeadNs) break;
            uint64_t wait_ns = item.timestamp - now - kMaxDeliveryLeadNs;
            if (wait_ns > 50000000ULL) wait_ns = 50000000ULL;
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait_ns));
        }
        if (!ctx->running.load()) break;

        // Sub-segment seek: drop frames before the requested moment. Segments
        // are the unit of transfer; they need not be the unit of seeking.
        {
            const long long skip = ctx->skip_until_pts_ns.load();
            if (skip >= 0) {
                long long base = ctx->seg_first_pts_ns.load();
                const long long pts = item.is_video ? item.video.pts_ns
                                                    : item.audio.pts_ns;
                if (base < 0) { ctx->seg_first_pts_ns = pts; base = pts; }
                if (pts - base < skip) continue;      // not there yet
                ctx->skip_until_pts_ns = -1;          // arrived
            }
        }

        // Keep the playing clock in step with the frame going to air, so the
        // displayed time advances continuously instead of once per segment.
        {
            long long base = ctx->seg_first_pts_ns.load();
            const long long pts = item.is_video ? item.video.pts_ns
                                                : item.audio.pts_ns;
            if (base < 0) { ctx->seg_first_pts_ns = pts; base = pts; }
            const long long segstart = ctx->seg_starts_at_ms.load();
            if (segstart > 0)
                ctx->playing_at_ms = segstart + (pts - base) / 1000000LL;
        }

        if (item.is_video) {
            const DecodedVideoFrame& f = item.video;
            struct obs_source_frame frame = {};
            frame.width  = (uint32_t)f.width;
            frame.height = (uint32_t)f.height;
            frame.format = VIDEO_FORMAT_I420;
            frame.timestamp = item.timestamp;
            // Plane pointers are recomputed here: the vector was copied, so the
            // pointers captured at decode time belong to the original buffer.
            uint8_t* base = const_cast<uint8_t*>(f.data.data());
            size_t off = 0;
            for (int i = 0; i < 3; ++i) {
                frame.data[i]     = base + off;
                frame.linesize[i] = (uint32_t)f.stride[i];
                off += (size_t)f.stride[i] * (i == 0 ? f.height : f.height / 2);
            }
            frame.full_range = f.full_range;
            video_format_get_parameters(VIDEO_CS_709,
                                        f.full_range ? VIDEO_RANGE_FULL
                                                     : VIDEO_RANGE_PARTIAL,
                                        frame.color_matrix,
                                        frame.color_range_min,
                                        frame.color_range_max);
            obs_source_output_video(ctx->source, &frame);
            ctx->frames_out++;
        } else {
            const DecodedAudioFrame& f = item.audio;
            struct obs_source_audio audio = {};
            audio.data[0] = reinterpret_cast<const uint8_t*>(f.interleaved.data());
            audio.frames  = f.frames;
            audio.speakers = ms_layout_for_channels(f.channels);
            audio.format   = AUDIO_FORMAT_FLOAT;      // interleaved float
            audio.samples_per_sec = (uint32_t)f.sample_rate;
            audio.timestamp = item.timestamp;
            obs_source_output_audio(ctx->source, &audio);
        }
    }
    mlog_info("source: delivery loop exiting");
}

// Anchors the playout clock on the first frame of EITHER stream and returns
// the reference pts. Audio must not wait for video here: anchoring on video
// only meant every audio frame decoded before the first video frame was
// dropped, which is audible as gaps and bursts (and it recurs on every
// decoder restart).
static int64_t anchor_pts(SourceCtx* ctx, int64_t pts_ns, bool is_video) {
    // Cushion covers the reordering window plus jitter.
    static constexpr uint64_t kPlayoutCushionNs = 500000000ULL;   // 500 ms
    int64_t first = ctx->first_pts_ns.load();
    if (first < 0) {
        ctx->first_pts_ns = pts_ns;
        ctx->playout_base_ns = os_gettime_ns() + kPlayoutCushionNs;
        first = pts_ns;
        mlog_info("source: playout anchored on first %s frame (pts %.3fs)",
                  is_video ? "video" : "audio", (double)pts_ns / 1e9);
    }
    return first;
}

static void deliver_video(SourceCtx* ctx, const DecodedVideoFrame& f) {
    if (!ctx->running.load()) return;
    const int64_t first = anchor_pts(ctx, f.pts_ns, true);

    PendingFrame item;
    item.is_video  = true;
    item.timestamp = ctx->playout_base_ns.load() + (uint64_t)(f.pts_ns - first);
    ctx->last_video_pts_ns = f.pts_ns;
    item.video     = f;              // owns its plane buffer (deep copy)

    ctx->width  = (uint32_t)f.width;
    ctx->height = (uint32_t)f.height;
    enqueue_frame(ctx, std::move(item));
}

static void deliver_audio(SourceCtx* ctx, const DecodedAudioFrame& f) {
    if (!ctx->running.load()) return;
    // Only the first audio track drives this source; further tracks are
    // exposed via companion sources in a later phase.
    if (f.track_index != 0) return;

    const int64_t first = anchor_pts(ctx, f.pts_ns, false);

    // Packed multi-channel guard. OBS resamples every source to its GLOBAL
    // layout (Settings -> Audio -> Channels). If the stream carries more
    // channels than that layout, OBS downmixes — which for packed audio means
    // the ISOs and click are summed into the programme and silently destroyed.
    // Say so loudly, once, rather than letting it pass.
    if (!ctx->checked_layout.exchange(true)) {
        struct obs_audio_info oai = {};
        if (obs_get_audio_info(&oai)) {
            const int global_ch = (int)get_audio_channels(oai.speakers);
            if (f.channels > global_ch) {
                mlog_error("stream carries %d audio channels but OBS is "
                           "configured for %d — the extra channels will be "
                           "DOWNMIXED and lost. Set Settings -> Audio -> "
                           "Channels to 7.1 on this machine.",
                           f.channels, global_ch);
            } else {
                mlog_info("audio: %d channel(s), OBS global layout %d channel(s)",
                          f.channels, global_ch);
            }
            ctx->audio_channels = f.channels;
        }
        if (ms_layout_for_channels(f.channels) == SPEAKERS_UNKNOWN)
            mlog_error("audio has %d channels, which has no OBS speaker "
                       "layout — 1,2,3,4,5,6 or 8 are supported",
                       f.channels);
    }

    PendingFrame item;
    item.is_video  = false;
    item.timestamp = ctx->playout_base_ns.load() + (uint64_t)(f.pts_ns - first);
    item.audio     = f;

    // Report the audio/video pts offset once: a large value here is the
    // signature of a stream-timing problem rather than a delivery problem.
    if (!ctx->logged_av_offset && ctx->last_video_pts_ns != 0) {
        ctx->logged_av_offset = true;
        mlog_info("source: audio/video pts offset %.3fs (should be near zero)",
                  (double)(f.pts_ns - ctx->last_video_pts_ns) / 1e9);
    }
    enqueue_frame(ctx, std::move(item));
}

// ── Worker loops ─────────────────────────────────────────────────────────────
static void poll_loop(SourceCtx* ctx) {
    mlog_info("source: poll loop started");
    int64_t next_poll = 0;
    while (ctx->running.load()) {
        const int64_t now = (int64_t)(os_gettime_ns() / 1000000ULL);

        if (now >= next_poll) {
            next_poll = now + ctx->poll_interval_ms;
            auto sess = get_session(ctx);
            if (!sess) break;
            // poll() does network I/O and can take seconds; never under a lock.
            RoomState st = sess->poll();
            if (st != ctx->last_room) {
                ctx->last_room = st;
                const char* name = st == RoomState::Live    ? "LIVE"
                                 : st == RoomState::Ended   ? "ended"
                                 : st == RoomState::Offline ? "offline"
                                                            : "unknown";
                mlog_info("source: room is %s%s", name,
                          st == RoomState::Offline ? " (encoder stopped or unreachable)" : "");
                if ((st == RoomState::Offline || st == RoomState::Ended) &&
                    !sess->last_error().empty())
                    mlog_warn("source: %s", sess->last_error().c_str());
            }
        }

        // Download-ahead runs continuously — this is what keeps filling the
        // cache while playback is paused or behind live.
        int fetched = 0;
        {
            auto sess = get_session(ctx);
            // Downloads block on the network — again, no lock held.
            // A larger batch is safe now that downloads do not hold the
            // state lock: it fills the buffer faster without affecting the UI.
            if (sess) fetched = sess->pump_downloads(8);
        }

        // Resume watchdog. A frozen picture after Resume is the failure that
        // is hardest to diagnose from a log, so say plainly whether frames
        // started flowing and, if not, what the state was.
        {
            const uint64_t r = ctx->resumed_at_ns.load();
            if (r != 0 && !ctx->resume_checked.load() &&
                os_gettime_ns() - r > 2000000000ULL) {
                ctx->resume_checked = true;
                const uint64_t delivered =
                    ctx->frames_out.load() - ctx->frames_at_resume.load();
                auto sw = get_session(ctx);
                size_t qdepth = 0;
                { std::lock_guard<std::mutex> qlk(ctx->dq_mtx); qdepth = ctx->dq.size(); }
                const bool at_edge = sw && sw->playback_head() > sw->live_edge();
                if (delivered == 0 && at_edge) {
                    // Not a fault: playback had caught right up, so there is
                    // simply nothing new yet. Say so plainly.
                    mlog_info("source: resumed at the live edge — waiting for "
                              "the main site to publish more (head=%llu "
                              "live=%llu)",
                              (unsigned long long)sw->playback_head(),
                              (unsigned long long)sw->live_edge());
                } else if (delivered == 0) {
                    mlog_error("source: RESUME FAILED — no frames delivered in "
                               "2s. paused=%d play_state=%d head=%llu live=%llu "
                               "queue=%zu buffered=%.1fs decoder=%d",
                               (int)ctx->paused.load(),
                               sw ? (int)sw->play_state() : -1,
                               sw ? (unsigned long long)sw->playback_head() : 0ULL,
                               sw ? (unsigned long long)sw->live_edge() : 0ULL,
                               qdepth,
                               sw ? sw->buffered_ahead_s() : 0.0,
                               (int)ctx->decoder_started.load());
                    if (sw && !sw->last_error().empty())
                        mlog_error("source: last error: %s",
                                   sw->last_error().c_str());
                } else {
                    mlog_info("source: resume delivered %llu frames in 2s — "
                              "playing", (unsigned long long)delivered);
                }
            }
        }

        // Periodic status so an operator can see it working.
        if (now - ctx->last_status_log_ms > 30000) {
            ctx->last_status_log_ms = now;
            auto sess2 = get_session(ctx);
            if (sess2) {
                auto& s = sess2->stats();
                mlog_info("source: head=%llu live=%llu behind=%.0fs "
                          "buffered=%.0fs cached=%zu downloaded=%llu "
                          "frames_out=%llu",
                          (unsigned long long)sess2->playback_head(),
                          (unsigned long long)sess2->live_edge(),
                          sess2->behind_live_s(),
                          sess2->buffered_ahead_s(),
                          sess2->cache().count(),
                          (unsigned long long)s.downloaded,
                          (unsigned long long)ctx->frames_out.load());
            }
        }

        // Always yield briefly, even when there is more to fetch. A tight
        // download loop starves the UI on Windows, where the running thread
        // is favoured for a contended lock; a few milliseconds costs nothing
        // against a segment download but guarantees the interface stays live.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(fetched == 0 ? 100 : 5));
    }
    mlog_info("source: poll loop exiting");
}

static void feed_loop(SourceCtx* ctx) {
    mlog_info("source: feed loop started");
    while (ctx->running.load()) {
        // While held, stop pulling and feeding entirely. Otherwise the decoder
        // and delivery queues fill, push_fragment blocks, and Resume cannot get
        // in — which froze OBS.
        if (ctx->paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Loading fills the buffer; only Play sends anything to air. This is
        // the difference between a feed that is ready and a feed that is out.
        if (!ctx->playing.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto sess = get_session(ctx);
        if (!sess) break;
        std::optional<PlayableSegment> seg;
        {
            if (sess->play_state() == PlayState::Stopped) sess->start();
            seg = sess->next_segment();
        }

        if (!seg) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // A jump (seek / jump-to-live / new event) means the next fragment
        // belongs to a different timeline. Tear the decoder down so it
        // restarts from the re-sent init segment; feeding across a jump
        // produces out-of-order timestamps and a glitched picture.
        {
            uint64_t d = sess->discontinuity_id();
            if (d != ctx->seen_discontinuity) {
                ctx->seen_discontinuity = d;
                if (ctx->decoder_started.load()) {
                    mlog_info("source: playback jumped — restarting decoder");
                    std::shared_ptr<CmafDecoder> old;
                    { std::lock_guard<std::mutex> lk(ctx->obj_mtx);
                      old = ctx->decoder; ctx->decoder.reset(); }
                    if (old) old->stop();      // stop() blocks: outside the lock
                    ctx->decoder_started = false;
                    ctx->first_pts_ns = -1;      // re-anchor the playout clock
                    ctx->logged_av_offset = false;
                    ctx->last_video_pts_ns = 0;
                    {
                        std::lock_guard<std::mutex> qlk(ctx->dq_mtx);
                        ctx->dq.clear();         // stale frames from the old timeline
                    }
                    ctx->dq_cv.notify_all();
                }
            }
        }

        // The init segment arrives with the first fragment; it must open the
        // decoder before any media is pushed.
        if (!ctx->decoder_started.load()) {
            if (seg->init.empty()) {
                mlog_error("source: first segment arrived without an init "
                           "segment — cannot start decoding");
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            auto dec = std::make_shared<CmafDecoder>();
            dec->on_video([ctx](const DecodedVideoFrame& f) { deliver_video(ctx, f); });
            dec->on_audio([ctx](const DecodedAudioFrame& f) { deliver_audio(ctx, f); });
            if (!dec->start(seg->init)) {
                mlog_error("source: decoder failed to start: %s",
                           dec->error().c_str());
                continue;
            }
            { std::lock_guard<std::mutex> lk(ctx->obj_mtx); ctx->decoder = dec; }
            ctx->decoder_started = true;
            ctx->feed_start_ns = os_gettime_ns();
            ctx->pushed_media_ns = 0;
            mlog_info("source: decoder started (init %zu bytes)",
                      seg->init.size());
        }

        // Feed at playout rate, keeping a small lead so the decoder always has
        // work but never runs seconds ahead of the wall clock.
        // Fragments must be decoded comfortably before their content is due,
        // or the first frames of each fragment arrive late (visible as a burst
        // of lateness at every fragment boundary).
        static constexpr uint64_t kFeedLeadNs = 2500000000ULL;   // 2.5 s
        while (ctx->running.load()) {
            const uint64_t elapsed = os_gettime_ns() - ctx->feed_start_ns;
            if (ctx->pushed_media_ns <= elapsed + kFeedLeadNs) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!ctx->running.load()) break;

        // Record this segment's wall time so delivered frames can be turned
        // into a clock reading, and carry any mid-segment seek offset.
        ctx->seg_starts_at_ms = (long long)seg->starts_at_ms;
        ctx->seg_first_pts_ns = -1;              // set by the first frame
        if (seg->skip_to_ms > 0)
            ctx->skip_until_pts_ns = seg->skip_to_ms * 1000000LL;

        // push_fragment blocks when the decoder is full — deliberately not
        // under any lock.
        if (auto dec = get_decoder(ctx)) dec->push_fragment(seg->media);
        ctx->pushed_media_ns += (uint64_t)(seg->duration_s * 1e9);
    }
    mlog_info("source: feed loop exiting");
}

// ── OBS callbacks ────────────────────────────────────────────────────────────
static const char* src_name(void*) { return obs_module_text("Multisite.Source"); }

static uint32_t src_width(void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    return ctx->width;
}
static uint32_t src_height(void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    return ctx->height;
}

static void stop_workers(SourceCtx* ctx) {
    ctx->flushing = true;             // release any blocked decoder callback
    if (!ctx->running.exchange(false)) { ctx->flushing = false; return; }
    ctx->dq_cv.notify_all();      // release anyone blocked on the queue
    if (ctx->poll_thread.joinable())    ctx->poll_thread.join();
    if (ctx->feed_thread.joinable())    ctx->feed_thread.join();
    if (ctx->deliver_thread.joinable()) ctx->deliver_thread.join();
    {
        std::lock_guard<std::mutex> lk(ctx->dq_mtx);
        ctx->dq.clear();
    }
    {
        std::shared_ptr<CmafDecoder> old;
        { std::lock_guard<std::mutex> lk(ctx->obj_mtx);
          old = ctx->decoder; ctx->decoder.reset(); }
        if (old) old->stop();          // blocks; outside the lock
    }
    ctx->decoder_started = false;
    ctx->first_pts_ns = -1;
    ctx->flushing = false;
}

static void src_update(void* data, obs_data_t* s) {
    auto* ctx = static_cast<SourceCtx*>(data);
    std::lock_guard<std::mutex> life(ctx->lifecycle_mtx);
    unregister_decoder_controls(ctx);
    stop_workers(ctx);

    // Storage is machine-wide (see decoder_settings.h): a source uses its own
    // fields when filled, otherwise the shared settings. That way credentials
    // are entered once, survive an unclean OBS exit, and are shared by every
    // additional source.
    DecoderSettings shared = decoder_settings();

    auto pick = [](const char* own, const std::string& fallback) {
        return (own && *own) ? std::string(own) : fallback;
    };

    S3Config s3;
    s3.endpoint_host     = pick(obs_data_get_string(s, S_ENDPOINT), shared.endpoint_host);
    s3.r2_account_id     = pick(obs_data_get_string(s, S_ACCOUNT),  shared.r2_account_id);
    s3.bucket            = pick(obs_data_get_string(s, S_BUCKET),   shared.bucket);
    s3.access_key_id     = pick(obs_data_get_string(s, S_KEYID),    shared.access_key_id);
    s3.secret_access_key = pick(obs_data_get_string(s, S_SECRET),   shared.secret_access_key);
    s3.region            = pick(obs_data_get_string(s, S_REGION),   shared.region);

    DecoderConfig dc;
    dc.room_id              = pick(obs_data_get_string(s, S_ROOM), shared.room_id);
    dc.prebuffer_segments   = (int)obs_data_get_int(s, S_PREBUF);
    dc.keep_behind_segments = (int)obs_data_get_int(s, S_KEEP);
    dc.buffer_minutes       = shared.buffer_minutes;
    ctx->poll_interval_ms   = (int)obs_data_get_int(s, S_POLL_MS);

    if (s3.bucket.empty() ||
        (s3.endpoint_host.empty() && s3.r2_account_id.empty())) {
        mlog_warn("source: not configured yet — enter storage details in the "
                  "Multisite Decoder dock (or this source's properties)");
        return;
    }

    // Anything typed into the source becomes the machine default, so the next
    // source (and the next OBS session) already has it.
    if (!shared.configured() ||
        shared.bucket != s3.bucket || shared.room_id != dc.room_id) {
        DecoderSettings upd = shared;
        upd.endpoint_host     = s3.endpoint_host;
        upd.r2_account_id     = s3.r2_account_id;
        upd.bucket            = s3.bucket;
        upd.access_key_id     = s3.access_key_id;
        upd.secret_access_key = s3.secret_access_key;
        upd.region            = s3.region;
        upd.room_id           = dc.room_id;
        upd.prebuffer_segments = dc.prebuffer_segments;
        upd.poll_interval_ms   = ctx->poll_interval_ms;
        upd.keep_behind_segments = dc.keep_behind_segments;
        set_decoder_settings(upd);
    }

    char* cachedir = obs_module_config_path("cache");
    dc.cache_dir = cachedir ? cachedir : "./multisite_cache";
    bfree(cachedir);

    {
        auto tx  = std::make_shared<S3Transport>(s3);
        auto ses = std::make_shared<DecoderSession>(dc, *tx);
        std::lock_guard<std::mutex> lk(ctx->obj_mtx);
        ctx->transport = tx;
        ctx->session   = ses;
    }

    register_decoder_controls(ctx);      // hotkeys act on this source
    ctx->running = true;
    ctx->deliver_thread = std::thread(deliver_loop, ctx);
    ctx->poll_thread    = std::thread(poll_loop, ctx);
    ctx->feed_thread    = std::thread(feed_loop, ctx);
    ctx->room_id_for_display = dc.room_id;
    mlog_info("source: watching room '%s' (prebuffer %d segments, poll %dms)",
              dc.room_id.c_str(), dc.prebuffer_segments, ctx->poll_interval_ms);
}

static void* src_create(obs_data_t* settings, obs_source_t* source) {
    auto* ctx = new SourceCtx();
    ctx->source = source;
    src_update(ctx, settings);
    return ctx;
}

static void src_destroy(void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    {
        std::lock_guard<std::mutex> life(ctx->lifecycle_mtx);
        unregister_decoder_controls(ctx);
        stop_workers(ctx);
    }
    {
        std::lock_guard<std::mutex> lk(ctx->obj_mtx);
        ctx->session.reset();
        ctx->transport.reset();
    }
    delete ctx;
}

static void src_defaults(obs_data_t* s) {
    obs_data_set_default_string(s, S_ROOM, "main-auditorium");
    obs_data_set_default_string(s, S_REGION, "auto");
    obs_data_set_default_int(s, S_POLL_MS, 3000);
    obs_data_set_default_int(s, S_PREBUF, 2);
    obs_data_set_default_int(s, S_KEEP, 200);
}

// ── DecoderControls: one implementation, shared by buttons and hotkeys ───────
// These are called from the UI thread. They must not block, and must not
// contend with a worker thread that is mid-network-call — so they take a
// reference to the session under a short lock and act through it.
void SourceCtx::pause() {
    auto sess = get_session(this);
    if (!sess) return;
    // Order matters: stop delivery first so the picture holds immediately,
    // then stop pulling new segments.
    paused = true;
    pause_started_ns = os_gettime_ns();
    sess->pause();
    mlog_info("source: PAUSED at segment %llu — cache keeps filling",
              (unsigned long long)sess->playback_head());
}

void SourceCtx::resume() {
    auto sess = get_session(this);
    if (!sess) { mlog_warn("source: resume ignored — no session"); return; }

    // Re-anchor rather than shift.
    //
    // The previous approach advanced the playout clock by the paused duration
    // and rewrote the timestamps of frames already queued. That has several
    // ways to fail quietly — if the pause timestamp was already consumed, if
    // the session was not actually in the Paused state, or if the stall
    // resync fired in between — and any of them leaves frames that never
    // become due, so the picture stays frozen.
    //
    // Jump-to-live works reliably because it treats the position change as a
    // discontinuity: drop what is queued and let the next decoded frame
    // re-anchor the clock. Resume now does the same. The cost is the handful
    // of already-decoded frames still in the queue (under half a second);
    // playback continues from the same point in the programme because the
    // decoder itself is not restarted.
    pause_started_ns = 0;

    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        dropped = dq.size();
        dq.clear();
    }
    first_pts_ns = -1;             // next frame re-anchors the playout clock
    dq_cv.notify_all();            // release the decoder if it was blocked

    paused = false;                // delivery and feeding resume at once
    sess->resume();

    resumed_at_ns = os_gettime_ns();
    frames_at_resume = frames_out.load();

    mlog_info("source: RESUMED at %.0fs behind live (state=%d, dropped %zu "
              "queued frames, clock re-anchoring)",
              sess->behind_live_s(), (int)sess->play_state(), dropped);
}

void SourceCtx::toggle_pause() {
    if (paused.load()) resume(); else pause();
}

void SourceCtx::jump_to_live() {
    auto sess = get_session(this);
    if (!sess) return;
    pause_started_ns = 0;
    sess->jump_to_live();
    paused = false;
    mlog_info("source: JUMPED TO LIVE (segment %llu)",
              (unsigned long long)sess->playback_head());
}

void SourceCtx::log_status() {
    auto sess = get_session(this);
    if (!sess) { mlog_info("source: not configured"); return; }
    auto& st = sess->stats();
    auto cur = sess->current_marker();
    mlog_info("source status: room=%d head=%llu live=%llu behind=%.0fs "
              "buffered=%.0fs cached=%zu downloaded=%llu dl_fail=%llu "
              "checksum_fail=%llu served=%llu frames_out=%llu%s%s",
              (int)sess->room_state(),
              (unsigned long long)sess->playback_head(),
              (unsigned long long)sess->live_edge(),
              sess->behind_live_s(),
              sess->buffered_ahead_s(),
              sess->cache().count(),
              (unsigned long long)st.downloaded,
              (unsigned long long)st.download_failures,
              (unsigned long long)st.checksum_failures,
              (unsigned long long)st.served,
              (unsigned long long)frames_out.load(),
              cur ? " marker=" : "",
              cur ? cur->label.c_str() : "");
    if (!sess->last_error().empty())
        mlog_info("source last error: %s", sess->last_error().c_str());
}

void SourceCtx::snapshot(DecoderSnapshot& out) const {
    out.room_id = room_id_for_display;
    out.paused  = paused.load();
    out.audio_channels = audio_channels.load();
    std::shared_ptr<DecoderSession> sess;
    { std::lock_guard<std::mutex> lk(obj_mtx); sess = session; }
    if (!sess) return;
    out.room_state       = (int)sess->room_state();
    out.head             = sess->playback_head();
    out.live_edge        = sess->live_edge();
    out.first_available  = sess->earliest_available();
    out.behind_live_s    = sess->behind_live_s();
    out.buffered_ahead_s = sess->buffered_ahead_s();
    out.cached           = sess->cache().count();
    out.last_error       = sess->last_error();
    {
        auto layout = sess->audio_layout();
        if (!layout.empty()) {
            out.audio_track_label = layout.front().label;
            out.channel_labels    = layout.front().channel_labels;
        }
    }
    out.playing = playing.load();
    out.locked  = controls_locked.load();
    // Prefer the frame-accurate playing clock; fall back to the segment.
    const long long tick = playing_at_ms.load();
    out.playhead_ms = tick > 0 ? tick : (long long)sess->playhead_wall_ms();
    {
        // Downloaded ranges as clock times, for the timeline.
        for (const auto& r : sess->cached_ranges()) {
            const int64_t a = sess->wall_clock_ms(r.first);
            const int64_t b = sess->wall_clock_ms(r.second);
            if (a > 0 && b >= a) out.cached_spans.emplace_back(a, b);
        }
    }
    out.live_ms     = sess->live_wall_ms();
    out.earliest_ms = sess->earliest_wall_ms();
    out.started_ms  = sess->event_started_ms();
    if (auto cur = sess->current_marker()) out.current_marker = cur->label;
    for (const auto& m : sess->markers())
        out.markers.push_back({ m.label, m.id, (long long)m.at_ms });
}

void SourceCtx::jump_to_marker(const std::string& id) {
    auto sess = get_session(this);
    if (!sess) return;
    if (!sess->jump_to_marker(id)) {
        mlog_warn("source: could not jump to marker '%s' — it may no longer "
                  "be retained", id.c_str());
        return;
    }
    pause_started_ns = 0;
    paused = false;
    mlog_info("source: jumped to marker (segment %llu)",
              (unsigned long long)sess->playback_head());
}

void SourceCtx::play() {
    auto sess = get_session(this);
    if (!sess) { mlog_warn("source: play ignored — not configured"); return; }
    pause_started_ns = 0;
    paused = false;
    playing = true;
    sess->resume();
    resumed_at_ns = os_gettime_ns();
    frames_at_resume = frames_out.load();
    resume_checked = false;
    mlog_info("source: PLAY — %.0fs behind live, %.0fs buffered",
              sess->behind_live_s(), sess->buffered_ahead_s());
}

void SourceCtx::stop_playback() {
    playing = false;
    paused = false;
    pause_started_ns = 0;
    flushing = true;
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        dq.clear();
    }
    flushing = false;
    first_pts_ns = -1;
    dq_cv.notify_all();
    mlog_info("source: STOPPED (still downloading, ready to play again)");
}

void SourceCtx::seek_to_time(long long wall_ms) {
    auto sess = get_session(this);
    if (!sess) return;
    const int64_t got = sess->seek_to_wall_ms((int64_t)wall_ms);
    if (got == 0) {
        mlog_warn("source: that moment is no longer available in storage");
        return;
    }
    // Treat as a discontinuity: drop what is queued and re-anchor. `flushing`
    // releases the decoder if it is waiting for queue space, so the restart
    // that follows can never block.
    flushing = true;
    {
        std::lock_guard<std::mutex> qlk(dq_mtx);
        dq.clear();
    }
    dq_cv.notify_all();
    flushing = false;
    first_pts_ns = -1;
    pause_started_ns = 0;
    paused = false;
    dq_cv.notify_all();
    mlog_info("source: went to %lld (%.0fs behind live)",
              (long long)got, sess->behind_live_s());
}

void SourceCtx::jog(double seconds) {
    const long long from = playing_at_ms.load();
    if (from <= 0) {
        mlog_warn("source: cannot jog until the playing time is known");
        return;
    }
    seek_to_time(from + (long long)(seconds * 1000.0));
}

void SourceCtx::set_delay_from_live(double seconds) {
    auto sess = get_session(this);
    if (!sess) return;
    const int64_t live = sess->live_wall_ms();
    if (live <= 0) {
        mlog_warn("source: live time not known yet");
        return;
    }
    seek_to_time((long long)live - (long long)(seconds * 1000.0));
    mlog_info("source: holding %.0f minutes behind live", seconds / 60.0);
}

void SourceCtx::reconfigure() {
    // obs_source_update with null settings re-applies the source's current
    // settings on the next tick, which re-runs src_update — picking up the
    // machine-wide storage config. Deferring avoids tearing down worker
    // threads from whichever thread happened to click the button.
    if (source) obs_source_update(source, nullptr);
}

void SourceCtx::seek(unsigned long long seq) {
    auto sess = get_session(this);
    if (!sess) return;
    if (!sess->seek((uint64_t)seq)) {
        mlog_warn("source: cannot seek to segment %llu (outside the retained "
                  "window)", seq);
        return;
    }
    pause_started_ns = 0;
    paused = false;
    mlog_info("source: seeked to segment %llu", seq);
}

// Property buttons delegate to the same methods the hotkeys use.
static bool on_pause(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->pause();  return false;
}
static bool on_resume(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->resume(); return false;
}
static bool on_jump_live(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->jump_to_live(); return false;
}
static bool on_status(obs_properties_t*, obs_property_t*, void* data) {
    static_cast<SourceCtx*>(data)->log_status(); return false;
}

// Jump to a marker published by the main site. The list is rebuilt whenever the
// properties dialog is opened, so it reflects whatever cues have been dropped.
static bool on_jump_marker(obs_properties_t*, obs_property_t* prop, void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    (void)prop;
    // Read the selection under the lock, then release it: jump_to_marker takes
    // the same lock itself.
    const std::string id = ctx->pending_marker_id;
    if (id.empty()) {
        mlog_info("source: pick a marker first");
        return false;
    }
    ctx->jump_to_marker(id);
    return false;
}

static bool on_marker_selected(void* data, obs_properties_t*,
                               obs_property_t*, obs_data_t* settings) {
    auto* ctx = static_cast<SourceCtx*>(data);
    ctx->pending_marker_id = obs_data_get_string(settings, "marker_id");
    return false;
}

static obs_properties_t* src_props(void* data) {
    obs_properties_t* p = obs_properties_create();
    obs_properties_add_text(p, S_ENDPOINT, obs_module_text("EndpointHost"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_ACCOUNT,  obs_module_text("R2AccountID"),  OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_BUCKET,   obs_module_text("Bucket"),       OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_KEYID,    obs_module_text("AccessKeyID"),  OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_SECRET,   obs_module_text("SecretKey"),    OBS_TEXT_PASSWORD);
    obs_properties_add_text(p, S_REGION,   obs_module_text("Region"),       OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_ROOM,     obs_module_text("RoomID"),       OBS_TEXT_DEFAULT);
    obs_properties_add_int_slider(p, S_PREBUF, obs_module_text("Prebuffer"), 0, 10, 1);
    obs_properties_add_int_slider(p, S_POLL_MS, obs_module_text("PollInterval"), 500, 10000, 500);
    obs_properties_add_int_slider(p, S_KEEP, obs_module_text("KeepBehind"), 10, 2000, 10);

    obs_properties_add_button(p, "btn_pause",  obs_module_text("Pause"),      on_pause);
    obs_properties_add_button(p, "btn_resume", obs_module_text("Resume"),     on_resume);
    obs_properties_add_button(p, "btn_live",   obs_module_text("JumpToLive"), on_jump_live);
    obs_properties_add_button(p, "btn_status", obs_module_text("LogStatus"),  on_status);

    // Markers published by the main site. Populated from the manifest the
    // decoder is already polling, so it lists the operator's own cue names.
    obs_property_t* mk = obs_properties_add_list(
        p, "marker_id", obs_module_text("JumpToMarker"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    if (data) {
        auto* ctx = static_cast<SourceCtx*>(data);
        if (auto sess = get_session(ctx)) {
            for (const auto& m : sess->markers()) {
                // Clock time, not a sequence number: an operator recognises
                // "10:42" and never "segment 147".
                char when[32] = "";
                if (m.at_ms > 0) {
                    const std::time_t t = (std::time_t)(m.at_ms / 1000);
                    std::tm lt{};
#if defined(_WIN32)
                    localtime_s(&lt, &t);
#else
                    localtime_r(&t, &lt);
#endif
                    std::strftime(when, sizeof(when), "%H:%M", &lt);
                }
                std::string label = when[0] ? (std::string(when) + "  " + m.label)
                                            : m.label;
                obs_property_list_add_string(mk, label.c_str(), m.id.c_str());
            }
        }
    }
    obs_property_set_modified_callback2(mk, on_marker_selected, data);
    obs_properties_add_button(p, "btn_jump_marker",
                              obs_module_text("JumpToMarkerButton"),
                              on_jump_marker);
    return p;
}

void register_source() {
    struct obs_source_info info = {};
    info.id           = "multisite_source";
    info.type         = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
                        OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name     = src_name;
    info.create       = src_create;
    info.destroy      = src_destroy;
    info.update       = src_update;
    info.get_defaults = src_defaults;
    info.get_properties = src_props;
    info.get_width    = src_width;
    info.get_height   = src_height;
    info.icon_type    = OBS_ICON_TYPE_MEDIA;
    obs_register_source(&info);
    mlog_info("registered source: multisite_source");
}

} // namespace multisite_obs
