// test_cmaf.cpp — feeds a real multi-track (1 video + 2 audio) source through
// CmafMuxer and writes init.mp4 + seg_*.m4s to disk for ffprobe validation.
//
// Source is produced by ffmpeg (see run script) at /tmp/src.mp4.
#include "../src/core/cmaf_muxer.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
}

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>

using namespace multisite;

static const AVRational NS_TB = {1, 1000000000};

static void write_file(const std::string& path, const std::vector<uint8_t>& b) {
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)b.data(), (std::streamsize)b.size());
}

int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "/tmp/src.mp4";
    const char* outdir = argc > 2 ? argv[2] : "/tmp/cmaf_out";

    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, src, nullptr, nullptr) < 0) {
        std::printf("FAIL: cannot open %s\n", src); return 2;
    }
    avformat_find_stream_info(in, nullptr);

    // Build muxer tracks from the input streams (preserve order).
    std::vector<CmafTrack> tracks;
    std::vector<int> in2track(in->nb_streams, -1);
    int audio_n = 0;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVCodecParameters* p = in->streams[i]->codecpar;
        CmafTrack t;
        t.codec_id = p->codec_id;
        if (p->extradata_size > 0)
            t.extradata.assign(p->extradata, p->extradata + p->extradata_size);
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            t.kind = CmafTrack::Video;
            t.width = p->width; t.height = p->height;
            t.fps_num = 30; t.fps_den = 1;
            t.label = "Main video";
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            t.kind = CmafTrack::Audio;
            t.sample_rate = p->sample_rate;
            t.channels = p->ch_layout.nb_channels;
            if (p->frame_size > 0) t.frame_size = p->frame_size;
            t.obs_track_idx = audio_n;
            t.label = audio_n == 0 ? "Main mix" : "ISO " + std::to_string(audio_n);
            ++audio_n;
        } else continue;
        in2track[i] = (int)tracks.size();
        tracks.push_back(std::move(t));
    }
    std::printf("source: %zu tracks (%d audio)\n", tracks.size(), audio_n);

    CmafMuxer mux(tracks, 2.0); // 2s target → expect 2 segments from a 4s source
    if (!mux.ok()) { std::printf("FAIL: muxer setup: %s\n", mux.error().c_str()); return 2; }

    std::string dir = outdir;
    std::error_code mkec; std::filesystem::create_directories(dir, mkec);
    write_file(dir + "/init.mp4", mux.init_segment());
    std::printf("init.mp4: %zu bytes\n", mux.init_segment().size());

    int seg_count = 0;
    mux.on_segment([&](uint64_t idx, std::vector<uint8_t> bytes, double dur, double pts) {
        char num[16];
        std::snprintf(num, sizeof(num), "%03llu", (unsigned long long)idx);
        write_file(dir + "/seg_" + num + ".m4s", bytes);
        std::printf("segment %llu: %zu bytes, dur=%.3fs, pts=%.3fs\n",
                    (unsigned long long)idx, bytes.size(), dur, pts);
        ++seg_count;
    });

    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(in, pkt) >= 0) {
        int ti = in2track[pkt->stream_index];
        if (ti >= 0) {
            AVRational tb = in->streams[pkt->stream_index]->time_base;
            CmafPacket cp;
            cp.track = ti;
            cp.data.assign(pkt->data, pkt->data + pkt->size);
            cp.pts_ns = pkt->pts != AV_NOPTS_VALUE ? av_rescale_q(pkt->pts, tb, NS_TB) : 0;
            cp.dts_ns = pkt->dts != AV_NOPTS_VALUE ? av_rescale_q(pkt->dts, tb, NS_TB) : cp.pts_ns;
            cp.duration_ns = av_rescale_q(pkt->duration, tb, NS_TB);
            cp.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            mux.push(cp);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    mux.flush();
    avformat_close_input(&in);

    std::printf("SEGMENTS=%d\n", seg_count);

    int failures = 0;
    auto check = [&](bool cond, const char* msg) {
        std::printf("  [%s] %s\n", cond ? "ok  " : "FAIL", msg);
        if (!cond) ++failures;
    };

    check(mux.ok(), "muxer reported no error");
    check(!mux.init_segment().empty(), "init segment produced");
    check(seg_count >= 2, "multiple media fragments produced");

    // Each media segment must start with a styp box (CMAF/DASH requirement).
    {
        const std::string seg0 = std::string(outdir) + "/seg_000.m4s";
        std::ifstream sf(seg0, std::ios::binary);
        char hdr[8] = {0};
        sf.read(hdr, 8);
        bool has_styp = (hdr[4] == 's' && hdr[5] == 't' &&
                         hdr[6] == 'y' && hdr[7] == 'p');
        check(has_styp, "segment begins with a styp box");
    }
    check(audio_n >= 2, "source carried multiple audio tracks");

    // Validate each fragment structurally (not by grepping log text, which
    // varies between FFmpeg versions): concatenate init+fragment, then use
    // ffprobe to count decoded video frames and audio streams.
    //
    // NOTE: all paths are std::string. Fixed-size char buffers are a trap here
    // because CI checkout paths can be very long and silently truncate.
    const std::string out = outdir;
    for (int i = 0; i < seg_count; ++i) {
        char idx[16];
        std::snprintf(idx, sizeof(idx), "%03d", i);
        const std::string seg  = out + "/seg_" + idx + ".m4s";
        const std::string chk  = out + "/_chk.mp4";
        const std::string fcnt = out + "/_frames.txt";

        if (system(("cat '" + out + "/init.mp4' '" + seg + "' > '" + chk + "'").c_str()) != 0) {
            check(false, "concat init+fragment");
            continue;
        }

        // Decoded video frames must be > 0 (proves the fragment really decodes).
        int probe_rc = system(("ffprobe -v error -count_frames -select_streams v "
                "-show_entries stream=nb_read_frames -of csv=p=0 '" + chk +
                "' 2>/dev/null > '" + fcnt + "'").c_str());
        (void)probe_rc;
        long frames = 0;
        if (FILE* f = fopen(fcnt.c_str(), "r")) {
            if (fscanf(f, "%ld", &frames) != 1) frames = 0;
            fclose(f);
        }
        std::printf("     (fragment %d: %ld decoded video frames)\n", i, frames);
        check(frames > 0, "fragment decodes to real video frames");

        // Every audio track must survive inside the fragment.
        const std::string acount =
            "test $(ffprobe -v error -select_streams a -show_entries stream=index "
            "-of csv=p=0 '" + chk + "' 2>/dev/null | wc -l) -eq " +
            std::to_string(audio_n);
        check(system(acount.c_str()) == 0, "all audio tracks present in fragment");
    }

    std::printf("\n%s\n", failures == 0 ? "CMAF MUXER TESTS PASSED"
                                        : "CMAF MUXER TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
