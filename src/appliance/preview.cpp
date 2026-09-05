#include "preview.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace multisite_player {

struct JpegEncoder::Impl {
    AVCodecContext* ctx = nullptr;
    SwsContext*     sws = nullptr;
    AVFrame*        frame = nullptr;
    AVPacket*       pkt = nullptr;
    int  width = 0, height = 0, quality = 0;
    int  src_w = 0, src_h = 0;

    ~Impl() { reset(); }

    void reset() {
        if (sws)   { sws_freeContext(sws); sws = nullptr; }
        if (frame) { av_frame_free(&frame); }
        if (pkt)   { av_packet_free(&pkt); }
        if (ctx)   { avcodec_free_context(&ctx); }
        width = height = src_w = src_h = 0;
    }

    // The encoder is rebuilt only when the shape of the job changes — a new
    // stream, or the browser asking for a different size. A preview at four
    // frames a second must not allocate a codec four times a second.
    bool ensure(int sw, int sh, int dw, int dh, int q, std::string& error) {
        if (ctx && sw == src_w && sh == src_h && dw == width && dh == height &&
            q == quality)
            return true;
        reset();

        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) { error = "this build of FFmpeg has no JPEG encoder"; return false; }

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) { error = "out of memory"; return false; }
        ctx->width = dw;
        ctx->height = dh;
        // MJPEG's full-range YUV variant. Anything else and the preview comes
        // out washed out compared with the picture going to air.
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->time_base = AVRational{1, 25};
        ctx->color_range = AVCOL_RANGE_JPEG;
        // FFmpeg's JPEG quality runs the other way round from the familiar
        // 1-100 scale, through the quantiser.
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        const int qscale = std::max(2, std::min(31, 32 - (q * 30) / 100));
        ctx->global_quality = FF_QP2LAMBDA * qscale;

        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            error = "could not start the JPEG encoder";
            reset();
            return false;
        }

        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !pkt) { error = "out of memory"; reset(); return false; }
        frame->format = ctx->pix_fmt;
        frame->width = dw;
        frame->height = dh;
        frame->quality = ctx->global_quality;
        if (av_frame_get_buffer(frame, 32) < 0) {
            error = "out of memory";
            reset();
            return false;
        }

        sws = sws_getContext(sw, sh, AV_PIX_FMT_YUV420P, dw, dh,
                             AV_PIX_FMT_YUVJ420P, SWS_BILINEAR,
                             nullptr, nullptr, nullptr);
        if (!sws) { error = "could not set up the scaler"; reset(); return false; }

        src_w = sw; src_h = sh; width = dw; height = dh; quality = q;
        return true;
    }
};

JpegEncoder::JpegEncoder() : d(new Impl) {}
JpegEncoder::~JpegEncoder() = default;

bool JpegEncoder::encode(const multisite::DecodedVideoFrame& f, int max_width,
                         int quality, std::vector<uint8_t>& out,
                         std::string& error) {
    out.clear();
    error.clear();
    if (f.width <= 0 || f.height <= 0 || f.data.empty()) {
        error = "nothing decoded yet";
        return false;
    }

    int dw = f.width, dh = f.height;
    if (max_width > 0 && f.width > max_width) {
        dw = max_width;
        dh = (int)((int64_t)f.height * max_width / f.width);
    }
    // The JPEG encoder wants even dimensions for 4:2:0.
    dw &= ~1; dh &= ~1;
    if (dw < 2 || dh < 2) { error = "picture is too small to preview"; return false; }

    if (!d->ensure(f.width, f.height, dw, dh, quality, error)) return false;

    const uint8_t* src[3] = { f.plane[0], f.plane[1], f.plane[2] };
    const int src_stride[3] = { f.stride[0], f.stride[1], f.stride[2] };
    if (av_frame_make_writable(d->frame) < 0) {
        error = "could not prepare the picture";
        return false;
    }
    sws_scale(d->sws, src, src_stride, 0, f.height, d->frame->data,
              d->frame->linesize);

    d->frame->pts = 0;
    if (avcodec_send_frame(d->ctx, d->frame) < 0) {
        error = "the JPEG encoder refused the picture";
        return false;
    }
    const int rc = avcodec_receive_packet(d->ctx, d->pkt);
    if (rc < 0) {
        error = "no JPEG came back";
        return false;
    }
    out.assign(d->pkt->data, d->pkt->data + d->pkt->size);
    av_packet_unref(d->pkt);
    return true;
}

} // namespace multisite_player
