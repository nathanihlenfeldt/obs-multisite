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

#include "../core/decoder_session.h"
#include "../core/cmaf_decoder.h"
#include "../core/s3_transport.h"

#include <atomic>
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

struct SourceCtx {
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
    uint64_t feed_start_ns = 0;      // wall clock when this decoder started
    uint64_t pushed_media_ns = 0;    // media duration handed over so far

    // status for logging
    std::atomic<uint64_t> frames_out{0};
    RoomState last_room = RoomState::Unknown;
    int64_t   last_status_log_ms = 0;
};

// ── Frame delivery ───────────────────────────────────────────────────────────
// OBS's async buffer only holds a fraction of a second (its audio buffering
// limit defaults to ~960 ms). Decoding a whole 6 s fragment produces 6 s of
// frames almost instantly, so delivery — not fragment feeding — is what has to
// be paced. Each frame is held until it is nearly due, which keeps OBS's queue
// small and playback smooth.
static constexpr uint64_t kMaxDeliveryLeadNs = 400000000ULL;   // 400 ms

static void wait_until_due(SourceCtx* ctx, uint64_t target_ns) {
    while (ctx->running.load()) {
        const uint64_t now = os_gettime_ns();
        if (target_ns <= now + kMaxDeliveryLeadNs) return;
        uint64_t wait_ns = target_ns - now - kMaxDeliveryLeadNs;
        if (wait_ns > 50000000ULL) wait_ns = 50000000ULL;   // 50 ms slices so
        std::this_thread::sleep_for(                        // shutdown stays
            std::chrono::nanoseconds(wait_ns));             // responsive
    }
}
static void deliver_video(SourceCtx* ctx, const DecodedVideoFrame& f) {
    if (!ctx->running.load()) return;

    // Anchor the playout clock on the first frame. A small cushion gives OBS
    // frames slightly in the future so its async buffering has something to
    // work with instead of presenting the moment each frame lands.
    static constexpr uint64_t kPlayoutCushionNs = 250000000ULL;   // 250 ms
    int64_t first = ctx->first_pts_ns.load();
    if (first < 0) {
        ctx->first_pts_ns = f.pts_ns;
        first = f.pts_ns;
        ctx->playout_base_ns = os_gettime_ns() + kPlayoutCushionNs;
    }

    struct obs_source_frame frame = {};
    frame.width  = (uint32_t)f.width;
    frame.height = (uint32_t)f.height;
    frame.format = VIDEO_FORMAT_I420;
    frame.timestamp = ctx->playout_base_ns.load() +
                      (uint64_t)(f.pts_ns - first);
    for (int i = 0; i < 3; ++i) {
        frame.data[i]     = f.plane[i];
        frame.linesize[i] = (uint32_t)f.stride[i];
    }
    frame.full_range = f.full_range;
    video_format_get_parameters(VIDEO_CS_709,
                                f.full_range ? VIDEO_RANGE_FULL
                                             : VIDEO_RANGE_PARTIAL,
                                frame.color_matrix,
                                frame.color_range_min,
                                frame.color_range_max);

    ctx->width  = frame.width;
    ctx->height = frame.height;
    wait_until_due(ctx, frame.timestamp);
    if (!ctx->running.load()) return;
    obs_source_output_video(ctx->source, &frame);
    ctx->frames_out++;
}

static void deliver_audio(SourceCtx* ctx, const DecodedAudioFrame& f) {
    if (!ctx->running.load()) return;
    // Only the first audio track drives this source; further tracks are
    // exposed via companion sources in a later phase.
    if (f.track_index != 0) return;

    int64_t first = ctx->first_pts_ns.load();
    if (first < 0) return;           // wait until video anchors the clock

    struct obs_source_audio audio = {};
    audio.data[0]        = reinterpret_cast<const uint8_t*>(f.interleaved.data());
    audio.frames         = f.frames;
    audio.speakers       = (f.channels >= 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;
    audio.format         = AUDIO_FORMAT_FLOAT;   // interleaved float
    audio.samples_per_sec = (uint32_t)f.sample_rate;
    audio.timestamp      = ctx->playout_base_ns.load() +
                           (uint64_t)(f.pts_ns - first);
    wait_until_due(ctx, audio.timestamp);
    if (!ctx->running.load()) return;
    obs_source_output_audio(ctx->source, &audio);
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
        static constexpr uint64_t kFeedLeadNs = 1500000000ULL;   // 1.5 s
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
    if (ctx->poll_thread.joinable()) ctx->poll_thread.join();
    if (ctx->feed_thread.joinable()) ctx->feed_thread.join();
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        if (ctx->decoder) { ctx->decoder->stop(); ctx->decoder.reset(); }
    }
    ctx->decoder_started = false;
    ctx->first_pts_ns = -1;
}

static void src_update(void* data, obs_data_t* s) {
    auto* ctx = static_cast<SourceCtx*>(data);
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

    ctx->running = true;
    ctx->poll_thread = std::thread(poll_loop, ctx);
    ctx->feed_thread = std::thread(feed_loop, ctx);
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

// Timeslipping buttons. Returning false means "properties layout unchanged".
static bool on_pause(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (ctx->session) {
        ctx->session->pause();
        mlog_info("source: PAUSED at segment %llu — cache keeps filling",
                  (unsigned long long)ctx->session->playback_head());
    }
    return false;
}
static bool on_resume(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (ctx->session) {
        ctx->session->resume();
        mlog_info("source: RESUMED from segment %llu (%.0fs behind live)",
                  (unsigned long long)ctx->session->playback_head(),
                  ctx->session->behind_live_s());
    }
    return false;
}
static bool on_jump_live(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (ctx->session) {
        ctx->session->jump_to_live();
        mlog_info("source: JUMPED TO LIVE (segment %llu)",
                  (unsigned long long)ctx->session->playback_head());
    }
    return false;
}
static bool on_status(obs_properties_t*, obs_property_t*, void* data) {
    auto* ctx = static_cast<SourceCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (!ctx->session) { mlog_info("source: not configured"); return false; }
    auto& s = ctx->session->stats();
    mlog_info("source status: room=%d head=%llu live=%llu behind=%.0fs "
              "buffered=%.0fs cached=%zu downloaded=%llu dl_fail=%llu "
              "checksum_fail=%llu served=%llu frames_out=%llu",
              (int)ctx->session->room_state(),
              (unsigned long long)ctx->session->playback_head(),
              (unsigned long long)ctx->session->live_edge(),
              ctx->session->behind_live_s(),
              ctx->session->buffered_ahead_s(),
              ctx->session->cache().count(),
              (unsigned long long)s.downloaded,
              (unsigned long long)s.download_failures,
              (unsigned long long)s.checksum_failures,
              (unsigned long long)s.served,
              (unsigned long long)ctx->frames_out.load());
    if (!ctx->session->last_error().empty())
        mlog_info("source last error: %s", ctx->session->last_error().c_str());
    return false;
}

static obs_properties_t* src_props(void*) {
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
