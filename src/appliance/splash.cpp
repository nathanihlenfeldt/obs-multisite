#include "splash.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <cstring>

namespace multisite_player {

namespace {

// A 5x7 font, written out so it can be read and checked rather than trusted.
// Capitals, digits and the punctuation an address needs — nothing else, which
// is why the splash speaks in capitals.
struct Glyph { char c; const char* rows[7]; };

const Glyph kFont[] = {
{' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
{'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
{'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
{'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
{'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
{'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
{'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
{'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
{'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
{'I', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"}},
{'J', {"....#", "....#", "....#", "....#", "#...#", "#...#", ".###."}},
{'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
{'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
{'M', {"#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"}},
{'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
{'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
{'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
{'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
{'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
{'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
{'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
{'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
{'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
{'W', {"#...#", "#...#", "#...#", "#...#", "#.#.#", "##.##", "#...#"}},
{'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
{'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
{'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
{'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
{'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
{'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
{'3', {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."}},
{'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
{'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
{'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}},
{'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
{'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
{'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
{'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
{',', {".....", ".....", ".....", ".....", ".##..", ".##..", ".#..."}},
{':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
{'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}},
{'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
{'(', {"..##.", ".#...", "#....", "#....", "#....", ".#...", "..##."}},
{')', {".##..", "...#.", "....#", "....#", "....#", "...#.", ".##.."}},
{'\'', {".##..", ".##..", ".#...", ".....", ".....", ".....", "....."}},
{'?', {".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."}},
{'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
{'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
{'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
{'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
{'@', {".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".###."}},
{'%', {"#...#", "#..#.", "...#.", "..#..", ".#...", ".#..#", "#...#"}},
{'#', {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."}},
};

const Glyph* glyph_for(char c) {
    const char up = (char)std::toupper((unsigned char)c);
    for (const auto& g : kFont) if (g.c == up) return &g;
    return nullptr;
}

// One column of spacing between characters, scaled with the text.
constexpr int kAdvance = 6;      // 5 wide + 1 gap

// A dark ground rather than black: a screen showing pure black in a lit room
// looks broken, and a campus wants to be able to tell "waiting" from "off".
constexpr uint32_t kBg      = 0x00121519;
constexpr uint32_t kText    = 0x00e8edf4;
constexpr uint32_t kDim     = 0x0093a1b5;
constexpr uint32_t kAccent  = 0x004a9de0;
constexpr uint32_t kWarn    = 0x00c8871d;
constexpr uint32_t kRule    = 0x002e3644;

} // namespace

Canvas::Canvas(int width, int height)
    : m_width(std::max(1, width)), m_height(std::max(1, height)),
      m_px((size_t)m_width * (size_t)m_height, 0) {}

void Canvas::fill(uint32_t bgrx) {
    std::fill(m_px.begin(), m_px.end(), bgrx);
}

void Canvas::rect(int x, int y, int w, int h, uint32_t bgrx) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(m_width, x + w), y1 = std::min(m_height, y + h);
    for (int row = y0; row < y1; ++row)
        std::fill(m_px.begin() + (size_t)row * m_width + x0,
                  m_px.begin() + (size_t)row * m_width + x1, bgrx);
}

int Canvas::text_width(const std::string& s, int scale) {
    if (s.empty()) return 0;
    return (int)s.size() * kAdvance * scale - scale;
}

void Canvas::text(int x, int y, const std::string& s, int scale,
                  uint32_t bgrx) {
    if (scale < 1) scale = 1;
    int pen = x;
    for (char c : s) {
        const Glyph* g = glyph_for(c);
        if (g) {
            for (int row = 0; row < 7; ++row) {
                const char* bits = g->rows[row];
                for (int col = 0; col < 5; ++col) {
                    if (bits[col] != '#') continue;
                    rect(pen + col * scale, y + row * scale, scale, scale, bgrx);
                }
            }
        }
        pen += kAdvance * scale;
    }
}

void Canvas::text_centred(int y, const std::string& s, int scale,
                          uint32_t bgrx) {
    text((m_width - text_width(s, scale)) / 2, y, s, scale, bgrx);
}

void render_splash(Canvas& canvas, const SplashInfo& info) {
    const int W = canvas.width(), H = canvas.height();
    canvas.fill(kBg);

    // Scale everything off the display height, so the same layout works on a
    // 720p monitor in an office and a 4K screen in an auditorium.
    const int unit  = std::max(1, H / 180);         // 6 at 1080p
    const int big   = unit * 2;
    const int mid   = unit;
    const int small = std::max(1, unit * 2 / 3);

    // The block is built before it is drawn, so its true height is known and
    // it can be centred. Laying it out as it goes left a screen weighted to
    // the top with a third of it empty.
    struct Line { std::string text; int scale; uint32_t colour; int gap_below; };
    std::vector<Line> lines;

    lines.push_back({info.hostname.empty() ? "CAMPUS PLAYER" : info.hostname,
                     big, kText, unit * 5});
    lines.push_back({"", 0, kRule, unit * 5});      // the rule

    if (info.addresses.empty()) {
        lines.push_back({"NO NETWORK CONNECTION", mid, kWarn, unit * 3});
        lines.push_back({"PLUG IN AN ETHERNET CABLE", small, kDim, unit * 4});
    } else {
        lines.push_back({"OPEN THIS ON A PHONE OR TABLET", small, kDim, unit * 3});
        for (const auto& a : info.addresses)
            lines.push_back({a, mid, kAccent, unit * 2});
        lines.back().gap_below = unit * 5;
    }

    if (!info.configured)
        lines.push_back({"THIS BOX HAS NO STORAGE DETAILS YET", small, kWarn,
                         unit * 3});
    else if (!info.room.empty())
        lines.push_back({"ROOM  " + info.room, small, kDim, unit * 3});

    if (!info.state.empty())
        lines.push_back({info.state, small, kText, unit * 2});
    if (!info.detail.empty())
        lines.push_back({info.detail, small, kDim, unit * 2});

    int total = 0;
    for (const auto& l : lines)
        total += (l.scale ? Canvas::text_height(l.scale)
                          : std::max(1, unit / 3)) + l.gap_below;
    total -= lines.empty() ? 0 : lines.back().gap_below;

    int y = std::max(unit * 4, (H - total) / 2);
    for (const auto& l : lines) {
        if (l.scale == 0) {
            const int thickness = std::max(1, unit / 3);
            canvas.rect(W / 6, y, W * 2 / 3, thickness, l.colour);
            y += thickness + l.gap_below;
            continue;
        }
        canvas.text_centred(y, l.text, l.scale, l.colour);
        y += Canvas::text_height(l.scale) + l.gap_below;
    }

    if (!info.version.empty()) {
        const std::string v = "MULTISITE PLAYER " + info.version;
        canvas.text((W - Canvas::text_width(v, small)) / 2,
                    H - Canvas::text_height(small) - unit * 4, v, small, kRule);
    }
}

bool load_still(const std::string& path, int width, int height, Canvas& out,
                std::string& error) {
    error.clear();
    if (path.empty()) { error = "no holding slide has been set"; return false; }

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "cannot open " + path;
        return false;
    }
    struct Closer {
        AVFormatContext** f;
        ~Closer() { if (*f) avformat_close_input(f); }
    } closer{&fmt};

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        error = path + " is not a picture this box can read";
        return false;
    }
    int stream = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream = (int)i;
            break;
        }
    }
    if (stream < 0) { error = path + " holds no picture"; return false; }

    AVCodecParameters* par = fmt->streams[stream]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) { error = "this build cannot read that picture format"; return false; }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) { error = "out of memory"; return false; }
    struct CtxCloser {
        AVCodecContext** c;
        ~CtxCloser() { if (*c) avcodec_free_context(c); }
    } ctx_closer{&ctx};

    if (avcodec_parameters_to_context(ctx, par) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        error = "could not decode " + path;
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    struct FrameCloser {
        AVPacket** p; AVFrame** f;
        ~FrameCloser() { if (*p) av_packet_free(p); if (*f) av_frame_free(f); }
    } frame_closer{&pkt, &frame};
    if (!pkt || !frame) { error = "out of memory"; return false; }

    bool got = false;
    while (!got && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream &&
            avcodec_send_packet(ctx, pkt) >= 0 &&
            avcodec_receive_frame(ctx, frame) >= 0)
            got = true;
        av_packet_unref(pkt);
    }
    if (!got) { error = "nothing could be decoded from " + path; return false; }

    // Fit inside the screen without distorting it: a slide stretched to a
    // shape it was not made in looks worse than one with a border.
    const double sx = (double)width / frame->width;
    const double sy = (double)height / frame->height;
    const double scale = std::min(sx, sy);
    int dw = std::max(2, (int)(frame->width * scale)) & ~1;
    int dh = std::max(2, (int)(frame->height * scale)) & ~1;

    std::vector<uint8_t> rgb((size_t)dw * dh * 4);
    SwsContext* sws = sws_getContext(frame->width, frame->height,
                                     (AVPixelFormat)frame->format, dw, dh,
                                     AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
    if (!sws) { error = "could not scale the holding slide"; return false; }
    uint8_t* dst[4] = { rgb.data(), nullptr, nullptr, nullptr };
    int dst_stride[4] = { dw * 4, 0, 0, 0 };
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst,
              dst_stride);
    sws_freeContext(sws);

    out = Canvas(width, height);
    out.fill(0x00000000);
    const int ox = (width - dw) / 2, oy = (height - dh) / 2;
    for (int row = 0; row < dh; ++row) {
        const uint8_t* src = rgb.data() + (size_t)row * dw * 4;
        for (int col = 0; col < dw; ++col) {
            const uint32_t px = (uint32_t)src[col * 4 + 0]
                              | ((uint32_t)src[col * 4 + 1] << 8)
                              | ((uint32_t)src[col * 4 + 2] << 16);
            out.rect(ox + col, oy + row, 1, 1, px);
        }
    }
    return true;
}

} // namespace multisite_player
