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

#include "../core/decoder_session.h"
#include "../core/cmaf_decoder.h"
#include "../core/s3_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

    std::unique_ptr<S3Transport>    transport;
    std::unique_ptr<DecoderSession> session;
    std::unique_ptr<CmafDecoder>    decoder;

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

    // DecoderControls — driven by hotkeys and the Tools menu.
    void pause() override;
    void resume() override;
    void toggle_pause() override;
    void jump_to_live() override;
    void log_status() override;

    // Marker chosen in the properties dialog, acted on by the Jump button.
    std::string pending_marker_id;
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
// Push a stamped frame for delivery. Blocks while the queue is full, which
// back-pressures the decoder rather than letting memory grow.
static void enqueue_frame(SourceCtx* ctx, PendingFrame&& item) {
    std::unique_lock<std::mutex> lk(ctx->dq_mtx);
    ctx->dq_cv.wait(lk, [ctx] {
        return ctx->dq.size() < kMaxQueuedFrames || !ctx->running.load();
    });
    if (!ctx->running.load()) return;
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
                const uint64_t behind = now - item.timestamp;
                ctx->playout_base_ns += behind;
                item.timestamp += behind;
                {
                    std::lock_guard<std::mutex> qlk(ctx->dq_mtx);
                    for (auto& q : ctx->dq) q.timestamp += behind;
                }
                mlog_warn("source: playout clock fell %.1fs behind (stall?) — "
                          "re-anchored", (double)behind / 1e9);
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
            RoomState st;
            {
                std::lock_guard<std::mutex> lk(ctx->mtx);
                if (!ctx->session) break;
                st = ctx->session->poll();
            }
            if (st != ctx->last_room) {
                ctx->last_room = st;
                const char* name = st == RoomState::Live    ? "LIVE"
                                 : st == RoomState::Ended   ? "ended"
                                 : st == RoomState::Offline ? "offline"
                                                            : "unknown";
                mlog_info("source: room is %s%s", name,
                          st == RoomState::Offline ? " (encoder stopped or unreachable)" : "");
                if (st == RoomState::Offline || st == RoomState::Ended) {
                    std::lock_guard<std::mutex> lk(ctx->mtx);
                    if (ctx->session && !ctx->session->last_error().empty())
                        mlog_warn("source: %s", ctx->session->last_error().c_str());
                }
            }
        }

        // Download-ahead runs continuously — this is what keeps filling the
        // cache while playback is paused or behind live.
        int fetched = 0;
        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            if (ctx->session) fetched = ctx->session->pump_downloads(4);
        }

        // Periodic status so an operator can see it working.
        if (now - ctx->last_status_log_ms > 30000) {
            ctx->last_status_log_ms = now;
            std::lock_guard<std::mutex> lk(ctx->mtx);
            if (ctx->session) {
                auto& s = ctx->session->stats();
                mlog_info("source: head=%llu live=%llu behind=%.0fs "
                          "buffered=%.0fs cached=%zu downloaded=%llu "
                          "frames_out=%llu",
                          (unsigned long long)ctx->session->playback_head(),
                          (unsigned long long)ctx->session->live_edge(),
                          ctx->session->behind_live_s(),
                          ctx->session->buffered_ahead_s(),
                          ctx->session->cache().count(),
                          (unsigned long long)s.downloaded,
                          (unsigned long long)ctx->frames_out.load());
            }
        }

        if (fetched == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    mlog_info("source: poll loop exiting");
}

static void feed_loop(SourceCtx* ctx) {
    mlog_info("source: feed loop started");
    while (ctx->running.load()) {
        std::optional<PlayableSegment> seg;
        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            if (!ctx->session) break;
            // Start playback as soon as the prebuffer is satisfied.
            if (ctx->session->play_state() == PlayState::Stopped)
                ctx->session->start();
            seg = ctx->session->next_segment();
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
            uint64_t d;
            {
                std::lock_guard<std::mutex> lk(ctx->mtx);
                d = ctx->session ? ctx->session->discontinuity_id() : 0;
            }
            if (d != ctx->seen_discontinuity) {
                ctx->seen_discontinuity = d;
                if (ctx->decoder_started.load()) {
                    mlog_info("source: playback jumped — restarting decoder");
                    std::lock_guard<std::mutex> lk(ctx->mtx);
                    if (ctx->decoder) { ctx->decoder->stop(); ctx->decoder.reset(); }
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
            std::lock_guard<std::mutex> lk(ctx->mtx);
            ctx->decoder = std::make_unique<CmafDecoder>();
            ctx->decoder->on_video([ctx](const DecodedVideoFrame& f) {
                deliver_video(ctx, f);
            });
            ctx->decoder->on_audio([ctx](const DecodedAudioFrame& f) {
                deliver_audio(ctx, f);
            });
            if (!ctx->decoder->start(seg->init)) {
                mlog_error("source: decoder failed to start: %s",
                           ctx->decoder->error().c_str());
                ctx->decoder.reset();
                continue;
            }
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

        {
            std::lock_guard<std::mutex> lk(ctx->mtx);
            if (ctx->decoder) ctx->decoder->push_fragment(seg->media);
        }
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
    if (!ctx->running.exchange(false)) return;
    ctx->dq_cv.notify_all();      // release anyone blocked on the queue
    if (ctx->poll_thread.joinable())    ctx->poll_thread.join();
    if (ctx->feed_thread.joinable())    ctx->feed_thread.join();
    if (ctx->deliver_thread.joinable()) ctx->deliver_thread.join();
    {
        std::lock_guard<std::mutex> lk(ctx->dq_mtx);
        ctx->dq.clear();
    }
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        if (ctx->decoder) { ctx->decoder->stop(); ctx->decoder.reset(); }
    }
    ctx->decoder_started = false;
    ctx->first_pts_ns = -1;
}

static void src_update(void* data, obs_data_t* s) {
    auto* ctx = static_cast<SourceCtx*>(data);
    unregister_decoder_controls(ctx);
    stop_workers(ctx);

    S3Config s3;
    s3.endpoint_host     = obs_data_get_string(s, S_ENDPOINT);
    s3.r2_account_id     = obs_data_get_string(s, S_ACCOUNT);
    s3.bucket            = obs_data_get_string(s, S_BUCKET);
    s3.access_key_id     = obs_data_get_string(s, S_KEYID);
    s3.secret_access_key = obs_data_get_string(s, S_SECRET);
    s3.region            = obs_data_get_string(s, S_REGION);

    DecoderConfig dc;
    dc.room_id              = obs_data_get_string(s, S_ROOM);
    dc.prebuffer_segments   = (int)obs_data_get_int(s, S_PREBUF);
    dc.keep_behind_segments = (int)obs_data_get_int(s, S_KEEP);
    ctx->poll_interval_ms   = (int)obs_data_get_int(s, S_POLL_MS);

    if (s3.bucket.empty() ||
        (s3.endpoint_host.empty() && s3.r2_account_id.empty())) {
        mlog_warn("source: not configured yet (need bucket + endpoint or "
                  "R2 account id)");
        return;
    }

    char* cachedir = obs_module_config_path("cache");
    dc.cache_dir = cachedir ? cachedir : "./multisite_cache";
    bfree(cachedir);

    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        ctx->transport = std::make_unique<S3Transport>(s3);
        ctx->session   = std::make_unique<DecoderSession>(dc, *ctx->transport);
    }

    register_decoder_controls(ctx);      // hotkeys act on this source
    ctx->running = true;
    ctx->deliver_thread = std::thread(deliver_loop, ctx);
    ctx->poll_thread    = std::thread(poll_loop, ctx);
    ctx->feed_thread    = std::thread(feed_loop, ctx);
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
    unregister_decoder_controls(ctx);
    stop_workers(ctx);
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
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
void SourceCtx::pause() {
    std::lock_guard<std::mutex> lk(mtx);
    if (!session) return;
    // Stop delivery immediately, then stop pulling new segments.
    paused = true;
    pause_started_ns = os_gettime_ns();
    session->pause();
    mlog_info("source: PAUSED at segment %llu — cache keeps filling",
              (unsigned long long)session->playback_head());
}

void SourceCtx::resume() {
    std::lock_guard<std::mutex> lk(mtx);
    if (!session) return;

    // The playout clock maps media time to wall time. While paused, wall time
    // keeps running but media time does not, so without this every frame would
    // be past due on resume and OBS would report audio lagging by the length
    // of the hold.
    const uint64_t paused_at = pause_started_ns.exchange(0);
    if (paused_at != 0) {
        const uint64_t paused_for = os_gettime_ns() - paused_at;
        playout_base_ns += paused_for;
        // Shift the frames held during the pause onto the new clock rather
        // than discarding them: that is the content the operator paused on.
        {
            std::lock_guard<std::mutex> qlk(dq_mtx);
            for (auto& q : dq) q.timestamp += paused_for;
        }
        dq_cv.notify_all();
        mlog_info("source: playout clock advanced %.1fs to cover the pause",
                  (double)paused_for / 1e9);
    }

    paused = false;               // delivery resumes at once
    session->resume();
    mlog_info("source: RESUMED from segment %llu (%.0fs behind live)",
              (unsigned long long)session->playback_head(),
              session->behind_live_s());
}

void SourceCtx::toggle_pause() {
    const bool was = paused.load();
    if (was) resume(); else pause();
}

void SourceCtx::jump_to_live() {
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!session) return;
        // A jump restarts the decoder and re-anchors the clock, so any pending
        // pause offset is irrelevant — and the operator expects picture back.
        pause_started_ns = 0;
        session->jump_to_live();
        mlog_info("source: JUMPED TO LIVE (segment %llu)",
                  (unsigned long long)session->playback_head());
    }
    paused = false;
}

void SourceCtx::log_status() {
    std::lock_guard<std::mutex> lk(mtx);
    if (!session) { mlog_info("source: not configured"); return; }
    auto& st = session->stats();
    auto cur = session->current_marker();
    mlog_info("source status: room=%d head=%llu live=%llu behind=%.0fs "
              "buffered=%.0fs cached=%zu downloaded=%llu dl_fail=%llu "
              "checksum_fail=%llu served=%llu frames_out=%llu%s%s",
              (int)session->room_state(),
              (unsigned long long)session->playback_head(),
              (unsigned long long)session->live_edge(),
              session->behind_live_s(),
              session->buffered_ahead_s(),
              session->cache().count(),
              (unsigned long long)st.downloaded,
              (unsigned long long)st.download_failures,
              (unsigned long long)st.checksum_failures,
              (unsigned long long)st.served,
              (unsigned long long)frames_out.load(),
              cur ? " marker=" : "",
              cur ? cur->label.c_str() : "");
    if (!session->last_error().empty())
        mlog_info("source last error: %s", session->last_error().c_str());
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
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (!ctx->session) return false;
    if (ctx->pending_marker_id.empty()) {
        mlog_info("source: pick a marker first");
        return false;
    }
    if (ctx->session->jump_to_marker(ctx->pending_marker_id)) {
        ctx->paused = false;
        ctx->pause_started_ns = 0;
        mlog_info("source: jumped to marker '%s' (segment %llu)",
                  ctx->pending_marker_id.c_str(),
                  (unsigned long long)ctx->session->playback_head());
    } else {
        mlog_warn("source: could not jump to marker '%s' — it may no longer "
                  "be retained", ctx->pending_marker_id.c_str());
    }
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
        std::lock_guard<std::mutex> lk(ctx->mtx);
        if (ctx->session) {
            for (const auto& m : ctx->session->markers()) {
                std::string label = m.label + "  (segment " +
                                    std::to_string(m.seq) + ")";
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
