#include "cmaf_decoder.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace multisite {

// Each fragment is decoded as an independent, SEEKABLE in-memory unit
// (init + fragment concatenated).
//
// This matters for A/V ordering. A non-seekable stream forces FFmpeg to return
// packets in file order, and a CMAF fragment stores each track's samples
// contiguously ([all video][all audio]) — so video floods out first and audio
// only appears a whole fragment later, which OBS reports as audio lagging by
// seconds. With a seekable buffer FFmpeg can order packets by DTS, so video and
// audio interleave correctly. Fragments are self-contained (each begins with a
// keyframe), so decoding them independently is safe and bounds memory to one
// fragment at a time.
static constexpr size_t kAvioBufSize = 1 << 16;
static constexpr size_t kMaxQueuedFragments = 4;

// Seekable read context over a fixed byte buffer.
struct MemReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    static int read(void* opaque, uint8_t* buf, int buf_size) {
        auto* r = static_cast<MemReader*>(opaque);
        if (r->pos >= r->size) return AVERROR_EOF;
        size_t n = std::min((size_t)buf_size, r->size - r->pos);
        std::memcpy(buf, r->data + r->pos, n);
        r->pos += n;
        return (int)n;
    }
    static int64_t seek(void* opaque, int64_t offset, int whence) {
        auto* r = static_cast<MemReader*>(opaque);
        if (whence == AVSEEK_SIZE) return (int64_t)r->size;
        int64_t np = 0;
        if (whence == SEEK_SET)      np = offset;
        else if (whence == SEEK_CUR) np = (int64_t)r->pos + offset;
        else if (whence == SEEK_END) np = (int64_t)r->size + offset;
        else return -1;
        if (np < 0 || np > (int64_t)r->size) return -1;
        r->pos = (size_t)np;
        return np;
    }
};

struct CmafDecoder::Impl {
    std::vector<uint8_t> init;

    std::deque<std::vector<uint8_t>> frags;
    mutable std::mutex      q_mtx;
    std::condition_variable q_cv;
    std::atomic<bool>       running{false};
    std::atomic<size_t>     queued_bytes_v{0};

    SwsContext* sws = nullptr;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;
    SwrContext* swr = nullptr;

    int width = 0, height = 0, audio_tracks = 0;

    VideoFrameCallback on_video;
    AudioFrameCallback on_audio;

    std::thread worker;
    bool ok_flag = true;
    std::string err;

    void fail(const std::string& m) { if (ok_flag) { ok_flag = false; err = m; } }

    void push(std::vector<uint8_t> bytes) {
        std::unique_lock<std::mutex> lk(q_mtx);
        q_cv.wait(lk, [this] {
            return frags.size() < kMaxQueuedFragments || !running.load();
        });
        if (!running.load()) return;
        queued_bytes_v += bytes.size();
        frags.push_back(std::move(bytes));
        lk.unlock();
        q_cv.notify_all();
    }

    void emit_video(AVFrame* f, AVRational tb) {
        if (!on_video) return;
        auto in_fmt = (AVPixelFormat)f->format;
        if (!sws || sws_w != f->width || sws_h != f->height || sws_fmt != in_fmt) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(f->width, f->height, in_fmt,
                                 f->width, f->height, AV_PIX_FMT_YUV420P,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws_w = f->width; sws_h = f->height; sws_fmt = in_fmt;
        }
        if (!sws) return;

        DecodedVideoFrame out;
        out.width = f->width; out.height = f->height;
        out.full_range = (f->color_range == AVCOL_RANGE_JPEG);
        int lines[4];
        av_image_fill_linesizes(lines, AV_PIX_FMT_YUV420P, f->width);
        size_t total = av_image_get_buffer_size(AV_PIX_FMT_YUV420P,
                                                f->width, f->height, 1);
        out.data.resize(total);
        uint8_t* dst[4] = { nullptr, nullptr, nullptr, nullptr };
        av_image_fill_pointers(dst, AV_PIX_FMT_YUV420P, f->height,
                               out.data.data(), lines);
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, lines);
        for (int i = 0; i < 3; ++i) { out.plane[i] = dst[i]; out.stride[i] = lines[i]; }
        int64_t pts = (f->pts == AV_NOPTS_VALUE) ? 0 : f->pts;
        out.pts_ns = (int64_t)(pts * av_q2d(tb) * 1e9);
        width = f->width; height = f->height;
        on_video(out);
    }

    void emit_audio(AVFrame* f, AVRational tb, int track_index) {
        if (!on_audio) return;
        const int out_ch = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : 2;
        if (!swr) {
            swr = swr_alloc();
            av_opt_set_chlayout(swr, "in_chlayout", &f->ch_layout, 0);
            av_opt_set_chlayout(swr, "out_chlayout", &f->ch_layout, 0);
            av_opt_set_int(swr, "in_sample_rate", f->sample_rate, 0);
            av_opt_set_int(swr, "out_sample_rate", f->sample_rate, 0);
            av_opt_set_sample_fmt(swr, "in_sample_fmt", (AVSampleFormat)f->format, 0);
            av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
            if (swr_init(swr) < 0) { swr_free(&swr); return; }
        }
        DecodedAudioFrame out;
        out.sample_rate = f->sample_rate;
        out.channels = out_ch;
        out.track_index = track_index;
        out.interleaved.resize((size_t)f->nb_samples * out_ch);
        uint8_t* dstp = reinterpret_cast<uint8_t*>(out.interleaved.data());
        int got = swr_convert(swr, &dstp, f->nb_samples,
                              (const uint8_t**)f->data, f->nb_samples);
        if (got <= 0) return;
        out.frames = (uint32_t)got;
        out.interleaved.resize((size_t)got * out_ch);
        int64_t pts = (f->pts == AV_NOPTS_VALUE) ? 0 : f->pts;
        out.pts_ns = (int64_t)(pts * av_q2d(tb) * 1e9);
        on_audio(out);
    }

    // Decode one fragment (init + fragment) as a seekable unit.
    void decode_unit(const std::vector<uint8_t>& frag) {
        std::vector<uint8_t> unit;
        unit.reserve(init.size() + frag.size());
        unit.insert(unit.end(), init.begin(), init.end());
        unit.insert(unit.end(), frag.begin(), frag.end());

        MemReader reader{ unit.data(), unit.size(), 0 };
        unsigned char* iobuf = (unsigned char*)av_malloc(kAvioBufSize);
        if (!iobuf) { fail("av_malloc"); return; }
        AVIOContext* avio = avio_alloc_context(iobuf, (int)kAvioBufSize, 0,
                                               &reader, &MemReader::read,
                                               nullptr, &MemReader::seek);
        if (!avio) { av_free(iobuf); fail("avio_alloc_context"); return; }

        AVFormatContext* fmt = avformat_alloc_context();
        if (!fmt) { av_freep(&avio->buffer); avio_context_free(&avio);
                    fail("avformat_alloc_context"); return; }
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
            avio_context_free(&avio);
            fail("avformat_open_input (invalid init or fragment?)");
            return;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt);
            if (avio) { if (avio->buffer) av_freep(&avio->buffer); avio_context_free(&avio); }
            fail("avformat_find_stream_info");
            return;
        }

        // Open a decoder per stream for this unit.
        std::vector<AVCodecContext*> ctxs(fmt->nb_streams, nullptr);
        std::vector<int> audio_idx(fmt->nb_streams, -1);
        int video_stream = -1, an = 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVCodecParameters* par = fmt->streams[i]->codecpar;
            const AVCodec* dec = avcodec_find_decoder(par->codec_id);
            if (!dec) continue;
            AVCodecContext* c = avcodec_alloc_context3(dec);
            if (!c) continue;
            avcodec_parameters_to_context(c, par);
            if (avcodec_open2(c, dec, nullptr) < 0) { avcodec_free_context(&c); continue; }
            ctxs[i] = c;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) video_stream = (int)i;
            else if (par->codec_type == AVMEDIA_TYPE_AUDIO) audio_idx[i] = an++;
        }
        audio_tracks = an;

        AVPacket* pkt = av_packet_alloc();
        AVFrame*  frm = av_frame_alloc();
        while (running.load() && av_read_frame(fmt, pkt) >= 0) {
            AVCodecContext* c = ctxs[pkt->stream_index];
            if (c && avcodec_send_packet(c, pkt) >= 0) {
                while (avcodec_receive_frame(c, frm) >= 0) {
                    AVRational tb = fmt->streams[pkt->stream_index]->time_base;
                    if ((int)pkt->stream_index == video_stream) emit_video(frm, tb);
                    else emit_audio(frm, tb, audio_idx[pkt->stream_index]);
                    av_frame_unref(frm);
                }
            }
            av_packet_unref(pkt);
        }
        // Flush the decoders so no frames are left behind in this unit.
        for (unsigned i = 0; i < ctxs.size() && running.load(); ++i) {
            if (!ctxs[i]) continue;
            avcodec_send_packet(ctxs[i], nullptr);
            while (avcodec_receive_frame(ctxs[i], frm) >= 0) {
                AVRational tb = fmt->streams[i]->time_base;
                if ((int)i == video_stream) emit_video(frm, tb);
                else emit_audio(frm, tb, audio_idx[i]);
                av_frame_unref(frm);
            }
        }
        av_frame_free(&frm);
        av_packet_free(&pkt);
        for (auto*& c : ctxs) if (c) avcodec_free_context(&c);
        avformat_close_input(&fmt);
        if (avio) { if (avio->buffer) av_freep(&avio->buffer); avio_context_free(&avio); }
    }

    void run() {
        while (running.load()) {
            std::vector<uint8_t> frag;
            {
                std::unique_lock<std::mutex> lk(q_mtx);
                q_cv.wait(lk, [this] { return !frags.empty() || !running.load(); });
                if (!running.load()) break;
                frag = std::move(frags.front());
                frags.pop_front();
                queued_bytes_v -= std::min(queued_bytes_v.load(), frag.size());
            }
            q_cv.notify_all();
            decode_unit(frag);
        }
    }

    ~Impl() {
        if (sws) sws_freeContext(sws);
        if (swr) swr_free(&swr);
    }
};

CmafDecoder::CmafDecoder() : d(std::make_unique<Impl>()) {}
CmafDecoder::~CmafDecoder() { stop(); }

void CmafDecoder::on_video(VideoFrameCallback cb) { d->on_video = std::move(cb); }
void CmafDecoder::on_audio(AudioFrameCallback cb) { d->on_audio = std::move(cb); }

bool CmafDecoder::start(const std::vector<uint8_t>& init_segment) {
    if (init_segment.empty()) { d->fail("empty init segment"); return false; }
    d->init = init_segment;
    d->running = true;
    d->worker = std::thread([this] { d->run(); });
    return true;
}

void CmafDecoder::push_fragment(const std::vector<uint8_t>& bytes) {
    if (!d->running.load()) return;
    d->push(bytes);
}

size_t CmafDecoder::queued_bytes() const { return d->queued_bytes_v.load(); }

void CmafDecoder::stop() {
    if (!d) return;
    if (!d->running.exchange(false)) {
        if (d->worker.joinable()) d->worker.join();
        return;
    }
    d->q_cv.notify_all();
    if (d->worker.joinable()) d->worker.join();
}

bool CmafDecoder::ok() const { return d->ok_flag; }
const std::string& CmafDecoder::error() const { return d->err; }
int CmafDecoder::video_width() const { return d->width; }
int CmafDecoder::video_height() const { return d->height; }
int CmafDecoder::audio_track_count() const { return d->audio_tracks; }

} // namespace multisite
