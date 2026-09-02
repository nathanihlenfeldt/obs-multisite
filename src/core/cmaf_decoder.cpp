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

// FFmpeg 7.0 (libavformat 61) made the AVIO read callback's buffer const-free
// in the same way as the writer; the read signature is stable, but keep the
// version gate style consistent with the muxer.
static constexpr size_t kAvioBufSize = 1 << 16;
static constexpr size_t kMaxQueued   = 64u * 1024u * 1024u;   // 64 MB back-pressure

struct CmafDecoder::Impl {
    // ── byte queue feeding AVIO ──────────────────────────────────────────────
    std::deque<uint8_t>      q;
    mutable std::mutex       q_mtx;
    std::condition_variable  q_cv;
    std::atomic<bool>        eof{false};
    std::atomic<bool>        running{false};

    // ── FFmpeg state ─────────────────────────────────────────────────────────
    AVFormatContext* fmt  = nullptr;
    AVIOContext*     avio = nullptr;
    SwsContext*      sws  = nullptr;
    int sws_w = 0, sws_h = 0, sws_fmt = -1;

    struct AudioTrack {
        int stream_index = -1;
        int track_index = 0;
        AVCodecContext* ctx = nullptr;
        SwrContext* swr = nullptr;
    };
    int video_stream = -1;
    AVCodecContext* vctx = nullptr;
    std::vector<AudioTrack> audio;

    int width = 0, height = 0;

    VideoFrameCallback on_video;
    AudioFrameCallback on_audio;

    std::thread worker;
    bool ok_flag = true;
    std::string err;

    void fail(const std::string& m) {
        if (ok_flag) { ok_flag = false; err = m; }
    }

    // Blocking read: hands FFmpeg whatever is queued, waiting when starved so
    // a live stream simply pauses rather than hitting a false EOF.
    static int read_cb(void* opaque, uint8_t* buf, int buf_size) {
        auto* self = static_cast<Impl*>(opaque);
        std::unique_lock<std::mutex> lk(self->q_mtx);
        self->q_cv.wait(lk, [self] {
            return !self->q.empty() || self->eof.load() || !self->running.load();
        });
        if (self->q.empty()) return AVERROR_EOF;

        int n = (int)std::min((size_t)buf_size, self->q.size());
        for (int i = 0; i < n; ++i) { buf[i] = self->q.front(); self->q.pop_front(); }
        lk.unlock();
        self->q_cv.notify_all();      // wake any blocked producer
        return n;
    }

    void push(const std::vector<uint8_t>& bytes) {
        std::unique_lock<std::mutex> lk(q_mtx);
        // Back-pressure instead of unbounded growth.
        q_cv.wait(lk, [this] {
            return q.size() < kMaxQueued || !running.load();
        });
        q.insert(q.end(), bytes.begin(), bytes.end());
        lk.unlock();
        q_cv.notify_all();
    }

    bool open_stream() {
        unsigned char* iobuf = (unsigned char*)av_malloc(kAvioBufSize);
        if (!iobuf) { fail("av_malloc"); return false; }
        avio = avio_alloc_context(iobuf, (int)kAvioBufSize, 0, this,
                                  &Impl::read_cb, nullptr, nullptr);
        if (!avio) { av_free(iobuf); fail("avio_alloc_context"); return false; }
        avio->seekable = 0;                 // live stream: no seeking backwards

        fmt = avformat_alloc_context();
        if (!fmt) { fail("avformat_alloc_context"); return false; }
        fmt->pb = avio;
        fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (avformat_open_input(&fmt, nullptr, nullptr, nullptr) < 0) {
            fail("avformat_open_input (is the init segment valid?)");
            return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            fail("avformat_find_stream_info");
            return false;
        }

        int audio_n = 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            AVCodecParameters* par = fmt->streams[i]->codecpar;
            const AVCodec* dec = avcodec_find_decoder(par->codec_id);
            if (!dec) continue;
            AVCodecContext* c = avcodec_alloc_context3(dec);
            if (!c) continue;
            avcodec_parameters_to_context(c, par);
            if (avcodec_open2(c, dec, nullptr) < 0) {
                avcodec_free_context(&c);
                continue;
            }
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && video_stream < 0) {
                video_stream = (int)i;
                vctx = c;
                width = par->width; height = par->height;
            } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                AudioTrack t;
                t.stream_index = (int)i;
                t.track_index = audio_n++;
                t.ctx = c;
                audio.push_back(t);
            } else {
                avcodec_free_context(&c);
            }
        }
        if (video_stream < 0) { fail("no decodable video stream"); return false; }
        return true;
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
        for (int i = 0; i < 3; ++i) {
            out.plane[i] = dst[i];
            out.stride[i] = lines[i];
        }
        int64_t pts = (f->pts == AV_NOPTS_VALUE) ? 0 : f->pts;
        out.pts_ns = (int64_t)(pts * av_q2d(tb) * 1e9);
        on_video(out);
    }

    void emit_audio(AudioTrack& t, AVFrame* f, AVRational tb) {
        if (!on_audio) return;
        const int out_ch = f->ch_layout.nb_channels > 0
                             ? f->ch_layout.nb_channels : 2;
        if (!t.swr) {
            t.swr = swr_alloc();
            av_opt_set_chlayout(t.swr, "in_chlayout", &f->ch_layout, 0);
            av_opt_set_chlayout(t.swr, "out_chlayout", &f->ch_layout, 0);
            av_opt_set_int(t.swr, "in_sample_rate", f->sample_rate, 0);
            av_opt_set_int(t.swr, "out_sample_rate", f->sample_rate, 0);
            av_opt_set_sample_fmt(t.swr, "in_sample_fmt",
                                  (AVSampleFormat)f->format, 0);
            av_opt_set_sample_fmt(t.swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
            if (swr_init(t.swr) < 0) { swr_free(&t.swr); return; }
        }
        DecodedAudioFrame out;
        out.sample_rate = f->sample_rate;
        out.channels = out_ch;
        out.track_index = t.track_index;
        out.interleaved.resize((size_t)f->nb_samples * out_ch);
        uint8_t* dstp = reinterpret_cast<uint8_t*>(out.interleaved.data());
        int got = swr_convert(t.swr, &dstp, f->nb_samples,
                              (const uint8_t**)f->data, f->nb_samples);
        if (got <= 0) return;
        out.frames = (uint32_t)got;
        out.interleaved.resize((size_t)got * out_ch);
        int64_t pts = (f->pts == AV_NOPTS_VALUE) ? 0 : f->pts;
        out.pts_ns = (int64_t)(pts * av_q2d(tb) * 1e9);
        on_audio(out);
    }

    void run() {
        if (!open_stream()) { running = false; return; }

        AVPacket* pkt = av_packet_alloc();
        AVFrame*  frm = av_frame_alloc();
        while (running.load()) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) {
                if (r == AVERROR_EOF) break;
                av_packet_unref(pkt);
                continue;
            }
            AVCodecContext* target = nullptr;
            AudioTrack* atrack = nullptr;
            if (pkt->stream_index == video_stream) {
                target = vctx;
            } else {
                for (auto& t : audio)
                    if (t.stream_index == pkt->stream_index) {
                        target = t.ctx; atrack = &t; break;
                    }
            }
            if (target && avcodec_send_packet(target, pkt) >= 0) {
                while (avcodec_receive_frame(target, frm) >= 0) {
                    AVRational tb = fmt->streams[pkt->stream_index]->time_base;
                    if (atrack) emit_audio(*atrack, frm, tb);
                    else        emit_video(frm, tb);
                    av_frame_unref(frm);
                }
            }
            av_packet_unref(pkt);
        }
        av_frame_free(&frm);
        av_packet_free(&pkt);
        running = false;
    }

    ~Impl() {
        if (vctx) avcodec_free_context(&vctx);
        for (auto& t : audio) {
            if (t.ctx) avcodec_free_context(&t.ctx);
            if (t.swr) swr_free(&t.swr);
        }
        if (sws) sws_freeContext(sws);
        if (fmt) {
            avformat_close_input(&fmt);      // frees fmt
        }
        if (avio) {
            if (avio->buffer) av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
    }
};

CmafDecoder::CmafDecoder() : d(std::make_unique<Impl>()) {}
CmafDecoder::~CmafDecoder() { stop(); }

void CmafDecoder::on_video(VideoFrameCallback cb) { d->on_video = std::move(cb); }
void CmafDecoder::on_audio(AudioFrameCallback cb) { d->on_audio = std::move(cb); }

bool CmafDecoder::start(const std::vector<uint8_t>& init_segment) {
    if (init_segment.empty()) { d->fail("empty init segment"); return false; }
    d->running = true;
    d->push(init_segment);
    d->worker = std::thread([this] { d->run(); });
    return true;
}

void CmafDecoder::push_fragment(const std::vector<uint8_t>& bytes) {
    if (!d->running.load()) return;
    d->push(bytes);
}

size_t CmafDecoder::queued_bytes() const {
    std::lock_guard<std::mutex> lk(d->q_mtx);
    return d->q.size();
}

void CmafDecoder::stop() {
    if (!d) return;
    if (!d->running.exchange(false)) {
        if (d->worker.joinable()) d->worker.join();
        return;
    }
    d->eof = true;
    d->q_cv.notify_all();
    if (d->worker.joinable()) d->worker.join();
}

bool CmafDecoder::ok() const { return d->ok_flag; }
const std::string& CmafDecoder::error() const { return d->err; }
int CmafDecoder::video_width() const { return d->width; }
int CmafDecoder::video_height() const { return d->height; }
int CmafDecoder::audio_track_count() const { return (int)d->audio.size(); }

} // namespace multisite
