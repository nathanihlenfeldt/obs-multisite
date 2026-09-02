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

#include <memory>
#include <string>
#include <vector>
#include <mutex>

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

struct OutputCtx {
    obs_output_t* output = nullptr;
    std::unique_ptr<S3Transport> transport;
    std::unique_ptr<Session>     session;
    std::unique_ptr<CmafMuxer>   muxer;

    // OBS encoder index → muxer track index
    int  video_track = -1;
    int  audio_track_for[MAX_AUDIO_MIXES];
    bool started = false;
    std::mutex mtx;
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
    return p;
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
    if (!ok) { mlog_error("failed to start session (check credentials)"); return false; }

    ctx->muxer->on_segment([ctx](uint64_t, std::vector<uint8_t> bytes,
                                 double dur, double pts) {
        ctx->session->publish_segment(std::move(bytes), dur, pts);
    });

    if (!obs_output_begin_data_capture(ctx->output, 0)) return false;
    ctx->started = true;
    mlog_info("multisite output started — room=%s event=%s",
              sc.room_id.c_str(), ctx->session->event_id().c_str());
    return true;
}

static void out_stop(void* data, uint64_t) {
    auto* ctx = static_cast<OutputCtx*>(data);
    std::lock_guard<std::mutex> lk(ctx->mtx);
    if (!ctx->started) return;
    obs_output_end_data_capture(ctx->output);
    if (ctx->muxer) ctx->muxer->flush();       // emit final fragment
    if (ctx->session) {
        auto st = ctx->session->status();
        mlog_info("stopping: %llu confirmed, %zu pending, %llu retries",
                  (unsigned long long)st.confirmed_total, st.pending,
                  (unsigned long long)st.retries);
        ctx->session->end();                    // drains spool, marks ended
    }
    ctx->started = false;
    ctx->session.reset(); ctx->muxer.reset(); ctx->transport.reset();
}

static void out_packet(void* data, struct encoder_packet* pkt) {
    auto* ctx = static_cast<OutputCtx*>(data);
    if (!ctx->started || !pkt || !ctx->muxer) return;

    int track = -1;
    if (pkt->type == OBS_ENCODER_VIDEO) track = ctx->video_track;
    else if (pkt->type == OBS_ENCODER_AUDIO &&
             pkt->track_idx < MAX_AUDIO_MIXES)
        track = ctx->audio_track_for[pkt->track_idx];
    if (track < 0) return;

    CmafPacket cp;
    cp.track = track;
    cp.data.assign(pkt->data, pkt->data + pkt->size);
    cp.pts_ns = pkt->pts;    // OBS packets are already in nanoseconds
    cp.dts_ns = pkt->dts;
    cp.keyframe = pkt->keyframe;
    ctx->muxer->push(cp);
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
    info.encoded_video_codecs = "h264;hevc";
    info.encoded_audio_codecs = "aac";
    obs_register_output(&info);
    mlog_info("registered output: multisite_output");
}

} // namespace multisite_obs
