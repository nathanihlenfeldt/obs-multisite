// multisite_output.cpp — the OBS output.
//
// Pulls encoded packets from OBS (H.264 video + up to 6 AAC audio tracks),
// muxes them into CMAF fragments, and hands each finished fragment to the
// Session, which spools it durably and uploads it with retry.
//
// Nothing here blocks the OBS encode thread on the network: publish_segment()
// only writes to the local durable spool.
//
#include <obs-module.h>
#include "plugin_log.h"

#include "../core/session.h"
#include "../core/cmaf_muxer.h"
#include "../core/s3_transport.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <atomic>
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
static constexpr char S_ENDPOINT[]  = "endpoint_host";
static constexpr char S_ACCOUNT[]   = "r2_account_id";
static constexpr char S_BUCKET[]    = "bucket";
static constexpr char S_KEYID[]     = "access_key_id";
static constexpr char S_SECRET[]    = "secret_access_key";
static constexpr char S_REGION[]    = "region";
static constexpr char S_ROOM[]      = "room_id";
static constexpr char S_SEGDUR[]    = "segment_duration_s";
static constexpr char S_TRACKLBL[]  = "track_labels";   // comma-separated
static constexpr char S_TAGS[]      = "use_object_tags";

// A finished fragment on its way from the muxer to the durable spool.
struct PendingFragment {
    std::vector<uint8_t> bytes;
    double duration_s = 0.0;
    double pts_offset_s = 0.0;
};

struct OutputCtx {
    obs_output_t* output = nullptr;
    std::unique_ptr<S3Transport> transport;
    std::unique_ptr<Session>     session;
    std::unique_ptr<CmafMuxer>   muxer;

    // OBS encoder index → muxer track index
    int  video_track = -1;
    int  audio_track_for[MAX_AUDIO_MIXES];
    std::mutex mtx;                  // guards start/stop transitions

    // Packets arrive on OBS's encoder threads while stop() runs on the UI
    // thread. `accepting` gates new packets and `mux_mtx` protects the muxer
    // itself, so the muxer can never be destroyed while it's being written to.
    std::atomic<bool> accepting{false};
    std::mutex        mux_mtx;
    bool              started = false;

    // Hashing a fragment and writing ~5 MB to disk must NOT happen on OBS's
    // encoder thread (it stalls audio). Fragments are handed to this writer
    // thread instead.
    std::deque<PendingFragment> wq;
    std::mutex                  wq_mtx;
    std::condition_variable     wq_cv;
    std::thread                 writer;
    std::atomic<bool>           writer_run{false};

    // diagnostics
    bool     logged_first_packets = false;
    uint64_t video_packets_seen = 0;
    uint64_t segments_muxed = 0;
    std::string last_verify_note;

    // progress logging state
    uint64_t last_logged_seq = 0;
    int64_t  last_log_ms = 0;
    LinkHealth last_health = LinkHealth::Healthy;
};

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == ',') { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t"); if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");  return s.substr(a, b - a + 1);
}

// Build muxer track configs from the OBS encoders actually attached.
static bool build_tracks(OutputCtx* ctx, std::vector<CmafTrack>& tracks,
                         VideoInfo& vinfo, std::vector<AudioTrack>& ainfo,
                         const std::string& label_csv) {
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i) ctx->audio_track_for[i] = -1;

    obs_encoder_t* venc = obs_output_get_video_encoder(ctx->output);
    if (!venc) { mlog_error("no video encoder attached"); return false; }

    CmafTrack vt;
    vt.kind = CmafTrack::Video;
    vt.codec_id = AV_CODEC_ID_H264;           // codec-agnostic hook: HEVC/AV1 later
    const char* vcodec = obs_encoder_get_codec(venc);
    if (vcodec && std::string(vcodec) == "hevc") vt.codec_id = AV_CODEC_ID_HEVC;
    vt.width  = (int)obs_encoder_get_width(venc);
    vt.height = (int)obs_encoder_get_height(venc);

    // SPS/PPS extradata
    uint8_t* hdr = nullptr; size_t hdr_size = 0;
    if (obs_encoder_get_extra_data(venc, &hdr, &hdr_size) && hdr && hdr_size)
        vt.extradata.assign(hdr, hdr + hdr_size);

    video_t* vid = obs_output_video(ctx->output);
    if (vid) {
        const struct video_output_info* voi = video_output_get_info(vid);
        if (voi && voi->fps_den) { vt.fps_num = (int)voi->fps_num; vt.fps_den = (int)voi->fps_den; }
    }
    ctx->video_track = 0;
    tracks.push_back(vt);

    vinfo.codec  = (vt.codec_id == AV_CODEC_ID_HEVC) ? "hevc" : "h264";
    vinfo.width  = vt.width; vinfo.height = vt.height;
    vinfo.fps    = vt.fps_den ? (double)vt.fps_num / vt.fps_den : 0.0;

    auto labels = split_csv(label_csv);
    int audio_n = 0;
    for (int i = 0; i < MAX_AUDIO_MIXES; ++i) {
        obs_encoder_t* aenc = obs_output_get_audio_encoder(ctx->output, (size_t)i);
        if (!aenc) continue;

        CmafTrack at;
        at.kind = CmafTrack::Audio;
        at.codec_id = AV_CODEC_ID_AAC;
        audio_t* aud = obs_encoder_audio(aenc);
        if (aud) {
            at.sample_rate = (int)audio_output_get_sample_rate(aud);
            at.channels    = (int)audio_output_get_channels(aud);
        }
        uint8_t* ah = nullptr; size_t ah_size = 0;
        if (obs_encoder_get_extra_data(aenc, &ah, &ah_size) && ah && ah_size)
            at.extradata.assign(ah, ah + ah_size);
        at.obs_track_idx = i;
        at.label = (audio_n < (int)labels.size() && !trim(labels[audio_n]).empty())
                     ? trim(labels[audio_n])
                     : ("Track " + std::to_string(i + 1));

        ctx->audio_track_for[i] = (int)tracks.size();

        AudioTrack meta;
        meta.idx = audio_n;
        meta.label = at.label;
        meta.codec = "aac";
        meta.channels = at.channels;
        meta.sample_rate = at.sample_rate;
        ainfo.push_back(meta);

        tracks.push_back(at);
        ++audio_n;
    }
    mlog_info("configured %d video + %d audio track(s)", 1, audio_n);
    return true;
}

// ── OBS callbacks ─────────────────────────────────────────────────────────────
static const char* out_name(void*) { return obs_module_text("Multisite.Output"); }

static void* out_create(obs_data_t*, obs_output_t* output) {
    auto* ctx = new OutputCtx();
    ctx->output = output;
    return ctx;
}
static void out_destroy(void* data) { delete static_cast<OutputCtx*>(data); }

static void out_defaults(obs_data_t* s) {
    obs_data_set_default_string(s, S_ROOM, "main-auditorium");
    obs_data_set_default_string(s, S_REGION, "auto");
    obs_data_set_default_double(s, S_SEGDUR, 6.0);
    obs_data_set_default_string(s, S_TRACKLBL, "Main mix,Sermon ISO,Click");
    obs_data_set_default_bool(s, S_TAGS, false);
}

static obs_properties_t* out_props(void*) {
    obs_properties_t* p = obs_properties_create();
    obs_properties_add_text(p, S_ENDPOINT, obs_module_text("EndpointHost"), OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_ACCOUNT,  obs_module_text("R2AccountID"),  OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_BUCKET,   obs_module_text("Bucket"),       OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_KEYID,    obs_module_text("AccessKeyID"),  OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_SECRET,   obs_module_text("SecretKey"),    OBS_TEXT_PASSWORD);
    obs_properties_add_text(p, S_REGION,   obs_module_text("Region"),       OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, S_ROOM,     obs_module_text("RoomID"),       OBS_TEXT_DEFAULT);
    obs_properties_add_float_slider(p, S_SEGDUR, obs_module_text("SegmentDuration"), 2.0, 15.0, 0.5);
    obs_properties_add_text(p, S_TRACKLBL, obs_module_text("TrackLabels"), OBS_TEXT_DEFAULT);
    // R2 rejects x-amz-tagging; leave off unless the store supports tagging.
    obs_properties_add_bool(p, S_TAGS, obs_module_text("UseObjectTags"));
    return p;
}

// Drains finished fragments into the durable spool, off the encoder thread.
static void writer_loop(OutputCtx* ctx) {
    for (;;) {
        PendingFragment f;
        {
            std::unique_lock<std::mutex> lk(ctx->wq_mtx);
            ctx->wq_cv.wait(lk, [ctx] {
                return !ctx->wq.empty() || !ctx->writer_run.load();
            });
            if (ctx->wq.empty()) {
                if (!ctx->writer_run.load()) return;   // asked to stop, nothing left
                continue;
            }
            f = std::move(ctx->wq.front());
            ctx->wq.pop_front();
        }
        // Checksum + durable write happen here, safely away from OBS threads.
        uint64_t seq = ctx->session->publish_segment(std::move(f.bytes),
                                                     f.duration_s, f.pts_offset_s);
        ctx->segments_muxed++;
        mlog_debug("segment %llu spooled (%.1fs)",
                   (unsigned long long)seq, f.duration_s);
    }
}

// Wait for queued fragments to reach the spool (bounded), then stop the writer.
static void writer_shutdown(OutputCtx* ctx, int timeout_ms = 10000) {
    if (!ctx->writer_run.load()) return;
    for (int waited = 0; waited < timeout_ms; waited += 25) {
        {
            std::lock_guard<std::mutex> lk(ctx->wq_mtx);
            if (ctx->wq.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ctx->writer_run = false;
    ctx->wq_cv.notify_all();
    if (ctx->writer.joinable()) ctx->writer.join();
}

static bool out_start(void* data) {
    auto* ctx = static_cast<OutputCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);

    obs_data_t* s = obs_output_get_settings(ctx->output);
    S3Config s3;
    s3.endpoint_host     = obs_data_get_string(s, S_ENDPOINT);
    s3.r2_account_id     = obs_data_get_string(s, S_ACCOUNT);
    s3.bucket            = obs_data_get_string(s, S_BUCKET);
    s3.access_key_id     = obs_data_get_string(s, S_KEYID);
    s3.secret_access_key = obs_data_get_string(s, S_SECRET);
    s3.region            = obs_data_get_string(s, S_REGION);

    SessionConfig sc;
    sc.room_id            = obs_data_get_string(s, S_ROOM);
    sc.segment_duration_s = obs_data_get_double(s, S_SEGDUR);
    std::string labels    = obs_data_get_string(s, S_TRACKLBL);
    sc.use_object_tags    = obs_data_get_bool(s, S_TAGS);
    obs_data_release(s);

    if (s3.bucket.empty() ||
        (s3.endpoint_host.empty() && s3.r2_account_id.empty())) {
        mlog_error("storage not configured (need bucket + endpoint or account id)");
        return false;
    }

    // Durable spool lives beside OBS's own config.
    char* cfgdir = obs_module_config_path("spool");
    sc.spool_dir = cfgdir ? cfgdir : "./multisite_spool";
    bfree(cfgdir);

    if (!obs_output_can_begin_data_capture(ctx->output, 0)) return false;
    if (!obs_output_initialize_encoders(ctx->output, 0))     return false;

    std::vector<CmafTrack> tracks;
    VideoInfo vinfo;
    std::vector<AudioTrack> ainfo;
    if (!build_tracks(ctx, tracks, vinfo, ainfo, labels)) return false;

    ctx->muxer = std::make_unique<CmafMuxer>(tracks, sc.segment_duration_s);
    if (!ctx->muxer->ok()) {
        mlog_error("muxer init failed: %s", ctx->muxer->error().c_str());
        return false;
    }

    ctx->transport = std::make_unique<S3Transport>(s3);
    ctx->session   = std::make_unique<Session>(sc, *ctx->transport);

    // Offer resume if a previous event was interrupted.
    auto resume = ctx->session->check_resumable();
    bool ok;
    if (resume.resumable) {
        mlog_info("resuming interrupted event %s (%zu segments pending)",
                  resume.event_id.c_str(), resume.pending_count);
        ok = ctx->session->resume(ctx->muxer->init_segment(), vinfo, ainfo);
    } else {
        ok = ctx->session->start_new(ctx->muxer->init_segment(), vinfo, ainfo);
    }
    if (!ok) {
        // Surface the actual HTTP failure rather than guessing.
        mlog_error("failed to start session: %s",
                   ctx->session->last_error().empty()
                       ? "no error recorded"
                       : ctx->session->last_error().c_str());
        // Probe the bucket so the operator learns whether it's credentials,
        // permissions, endpoint, or something request-specific.
        std::string probe = ctx->transport->self_test();
        if (probe.empty())
            mlog_error("connectivity probe SUCCEEDED — credentials and bucket are "
                       "fine, so the failure is request-specific (see above)");
        else
            mlog_error("connectivity probe also failed: %s", probe.c_str());
        return false;
    }

    // Log upload progress so the operator can see it working without going
    // and inspecting the bucket. Chatty for the first few segments (so you
    // get quick confirmation), then once every ~30s.
    ctx->session->set_progress_callback([ctx](const Session::Status& st) {
        int64_t now = now_ms();
        bool early   = st.confirmed_total <= 3;
        bool overdue = (now - ctx->last_log_ms) > 30000;
        bool health_changed = st.health != ctx->last_health;

        if (early || overdue || health_changed) {
            const char* h = st.health == LinkHealth::Healthy  ? "healthy"
                          : st.health == LinkHealth::Degraded ? "DEGRADED"
                                                              : "OFFLINE";
            mlog_info("uploaded segment %llu — %llu confirmed, %.1f MB, "
                      "%zu queued, %llu retries, link %s",
                      (unsigned long long)st.last_confirmed,
                      (unsigned long long)st.confirmed_total,
                      (double)st.bytes_uploaded / (1024.0 * 1024.0),
                      st.pending,
                      (unsigned long long)st.retries, h);
            ctx->last_log_ms = now;
            ctx->last_health = st.health;
        }

        // Report the store-side verification of early uploads. A store that
        // returns success without persisting is otherwise invisible.
        if (!st.verify_note.empty() && st.verify_note != ctx->last_verify_note) {
            ctx->last_verify_note = st.verify_note;
            if (st.verify_failures > 0)
                mlog_error("UPLOAD VERIFICATION FAILED: %s", st.verify_note.c_str());
            else
                mlog_info("upload verified in bucket: %s", st.verify_note.c_str());
        }
        // Always warn when the link degrades — that's the thing an operator
        // must know about mid-service.
        if (health_changed && st.health != LinkHealth::Healthy) {
            mlog_warn("upload link %s — capture continues, segments are "
                      "queued to disk and will be sent when it recovers",
                      st.health == LinkHealth::Degraded ? "degraded" : "offline");
        }
    });

    // Hand finished fragments to the writer thread. This callback runs on an
    // OBS encoder thread, so it must stay cheap — no hashing, no disk I/O.
    ctx->muxer->on_segment([ctx](uint64_t, std::vector<uint8_t> bytes,
                                 double dur, double pts) {
        PendingFragment f;
        f.bytes = std::move(bytes);
        f.duration_s = dur;
        f.pts_offset_s = pts;
        {
            std::lock_guard<std::mutex> lk(ctx->wq_mtx);
            ctx->wq.push_back(std::move(f));
        }
        ctx->wq_cv.notify_one();
    });

    ctx->writer_run = true;
    ctx->writer = std::thread(writer_loop, ctx);

    if (!obs_output_begin_data_capture(ctx->output, 0)) {
        writer_shutdown(ctx);
        return false;
    }
    ctx->started = true;
    ctx->accepting = true;      // packets may now enter the muxer
    mlog_info("multisite output started — room=%s event=%s",
              sc.room_id.c_str(), ctx->session->event_id().c_str());
    return true;
}

static void out_stop(void* data, uint64_t) {
    auto* ctx = static_cast<OutputCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (!ctx->started) return;

    // ORDER MATTERS. Packets arrive on OBS's encoder threads; stop runs on the
    // caller's thread. Close the gate first, let OBS stop delivering, and only
    // then touch the muxer — otherwise the muxer can be destroyed mid-write
    // (a use-after-free that crashes inside avformat).
    ctx->accepting = false;
    obs_output_end_data_capture(ctx->output);

    {
        std::lock_guard<std::mutex> mlk(ctx->mux_mtx);
        if (ctx->muxer) ctx->muxer->flush();   // emits the final fragment
        ctx->muxer.reset();                    // safe: no packet can be inside
    }

    // Make sure queued fragments reach the durable spool before draining.
    writer_shutdown(ctx);

    if (ctx->session) {
        auto st = ctx->session->status();
        mlog_info("stopping: %llu confirmed, %zu pending, %llu retries, "
                  "%llu segments muxed",
                  (unsigned long long)st.confirmed_total, st.pending,
                  (unsigned long long)st.retries,
                  (unsigned long long)ctx->segments_muxed);
        if (ctx->segments_muxed == 0)
            mlog_warn("no segments were produced — check that the video "
                      "encoder's keyframe interval is <= the segment duration");
        ctx->session->end();                    // drains spool, marks ended
    }
    ctx->started = false;
    ctx->session.reset(); ctx->muxer.reset(); ctx->transport.reset();
}

// Lets OBS (and scripts via obs_output_get_total_bytes) show upload volume.
static uint64_t out_total_bytes(void* data) {
    auto* ctx = static_cast<OutputCtx*>(data);
    return (ctx && ctx->session) ? ctx->session->bytes_uploaded() : 0;
}

// OBS timestamps are in the ENCODER's timebase (e.g. 1/30 for 30fps video,
// 1/48000 for audio) — NOT nanoseconds. Convert explicitly; getting this wrong
// silently breaks segmentation, because the muxer's "have we reached the target
// duration?" test never becomes true.
static inline int64_t ts_to_ns(int64_t ts, int32_t tb_num, int32_t tb_den) {
    if (tb_den <= 0) tb_den = 1;
    if (tb_num <= 0) tb_num = 1;
    // ts * (tb_num / tb_den) seconds → nanoseconds. Split the multiply to
    // avoid overflow without needing 128-bit math (MSVC has no __int128).
    const int64_t ns_per_unit = 1000000000LL * (int64_t)tb_num / (int64_t)tb_den;
    const int64_t rem         = 1000000000LL * (int64_t)tb_num % (int64_t)tb_den;
    return ts * ns_per_unit + (ts * rem) / (int64_t)tb_den;
}

static void out_packet(void* data, struct encoder_packet* pkt) {
    auto* ctx = static_cast<OutputCtx*>(data);
    // Cheap gate first: once stop() begins, packets are dropped immediately.
    if (!ctx || !pkt || !ctx->accepting.load()) return;

    int track = -1;
    if (pkt->type == OBS_ENCODER_VIDEO) track = ctx->video_track;
    else if (pkt->type == OBS_ENCODER_AUDIO &&
             pkt->track_idx < MAX_AUDIO_MIXES)
        track = ctx->audio_track_for[pkt->track_idx];
    if (track < 0) return;

    CmafPacket cp;
    cp.track = track;
    cp.data.assign(pkt->data, pkt->data + pkt->size);
    cp.pts_ns = ts_to_ns(pkt->pts, pkt->timebase_num, pkt->timebase_den);
    cp.dts_ns = ts_to_ns(pkt->dts, pkt->timebase_num, pkt->timebase_den);
    cp.keyframe = pkt->keyframe;

    // One-time sanity log: confirms the timebase conversion is sane and that
    // keyframes are arriving (both are prerequisites for segmentation).
    if (!ctx->logged_first_packets && pkt->type == OBS_ENCODER_VIDEO) {
        ctx->video_packets_seen++;
        if (ctx->video_packets_seen <= 2 || pkt->keyframe) {
            mlog_info("video pkt: pts=%lld tb=%d/%d -> %.3fs%s",
                      (long long)pkt->pts, (int)pkt->timebase_num,
                      (int)pkt->timebase_den, (double)cp.pts_ns / 1e9,
                      pkt->keyframe ? " [KEYFRAME]" : "");
            if (pkt->keyframe && ctx->video_packets_seen > 2)
                ctx->logged_first_packets = true;   // seen enough
        }
    }

    // Hold the muxer lock only for the push itself. stop() takes this same
    // lock before destroying the muxer, so this can never touch freed memory.
    {
        std::lock_guard<std::mutex> mlk(ctx->mux_mtx);
        if (!ctx->muxer) return;               // stop() got here first
        ctx->muxer->push(cp);
    }
}

void register_output() {
    struct obs_output_info info = {};
    info.id             = "multisite_output";
    info.flags          = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK;
    info.get_name       = out_name;
    info.create         = out_create;
    info.destroy        = out_destroy;
    info.start          = out_start;
    info.stop           = out_stop;
    info.encoded_packet = out_packet;
    info.get_properties = out_props;
    info.get_defaults   = out_defaults;
    info.get_total_bytes = out_total_bytes;
    info.encoded_video_codecs = "h264;hevc";
    info.encoded_audio_codecs = "aac";
    obs_register_output(&info);
    mlog_info("registered output: multisite_output");
}

} // namespace multisite_obs
