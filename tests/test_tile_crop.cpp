// SPDX-License-Identifier: GPL-3.0-or-later
// test_tile_crop.cpp — one tile of a composited feed, as a view.
//
// tile_view() is the appliance's half of Phase 10: the OBS source gets a tile
// by offsetting obs_source_frame's plane pointers, and the appliance — which
// decodes the whole picture and hands one frame to an HDMI output — needs the
// same thing spelled out. It is worth pinning because every way of getting it
// wrong is quiet: a view that starts half a cell off still shows a picture, of
// the wrong region, and one that dropped the original stride still shows a
// picture, sheared.
#include "../src/core/tile_crop.h"

#include <cstdio>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// A synthetic I420 frame with tight strides, so an offset is trivially
// checkable: plane 0 = w x h, each chroma plane = ceil(w/2) x ceil(h/2).
static DecodedVideoFrame make_frame(int w, int h) {
    DecodedVideoFrame f;
    f.width  = w;
    f.height = h;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    f.stride[0] = w;
    f.stride[1] = cw;
    f.stride[2] = cw;
    const size_t ysz = (size_t)f.stride[0] * (size_t)h;
    const size_t csz = (size_t)f.stride[1] * (size_t)ch;
    f.data.assign(ysz + 2 * csz, 0);
    f.plane[0] = f.data.data();
    f.plane[1] = f.plane[0] + ysz;
    f.plane[2] = f.plane[1] + csz;
    return f;
}

int main() {
    std::printf("== a tile is a view into the same buffer ==\n");
    {
        DecodedVideoFrame f = make_frame(3840, 1080);
        f.pts_ns = 123456;
        const TileLayout lay = TileLayout::parse("2x1");

        const DecodedVideoFrame t0 = tile_view(f, lay, 0);
        CHECK(t0.width == 1920 && t0.height == 1080, "tile 0 is 1920x1080");
        CHECK(t0.plane[0] == f.plane[0], "tile 0 starts at the top-left");
        CHECK(t0.stride[0] == f.stride[0], "and keeps the original stride");
        CHECK(t0.data.empty(), "the view owns no buffer — nothing was copied");
        CHECK(t0.pts_ns == 123456, "the timestamp is carried through");

        const DecodedVideoFrame t1 = tile_view(f, lay, 1);
        CHECK(t1.width == 1920 && t1.height == 1080, "tile 1 is the other half");
        CHECK(t1.plane[0] == f.plane[0] + 1920, "tile 1 starts 1920 across");
        // Chroma is half resolution, so the same edge is half the samples in.
        CHECK(t1.plane[1] == f.plane[1] + 960, "its chroma starts 960 across");
        CHECK(t1.plane[2] == f.plane[2] + 960, "in both chroma planes");
    }

    std::printf("== a 2x2 tile is offset in both directions ==\n");
    {
        DecodedVideoFrame f = make_frame(1920, 1080);
        const TileLayout lay = TileLayout::parse("2x2");

        const DecodedVideoFrame t2 = tile_view(f, lay, 2);   // bottom-left
        CHECK(t2.width == 960 && t2.height == 540, "bottom-left is 960x540");
        CHECK(t2.plane[0] == f.plane[0] + (size_t)540 * f.stride[0],
              "luma starts at row 540");
        CHECK(t2.plane[1] == f.plane[1] + (size_t)270 * f.stride[1],
              "chroma starts at row 270");

        const DecodedVideoFrame t3 = tile_view(f, lay, 3);   // bottom-right
        CHECK(t3.plane[0] == f.plane[0] + (size_t)540 * f.stride[0] + 960,
              "luma is one cell right and 540 down");
        CHECK(t3.plane[1] == f.plane[1] + (size_t)270 * f.stride[1] + 480,
              "chroma is half a cell right and 270 down");
    }

    std::printf("== the whole picture is the safe fallback ==\n");
    {
        DecodedVideoFrame f = make_frame(1920, 1080);

        const DecodedVideoFrame whole = tile_view(f, TileLayout::parse("1x1"), 0);
        CHECK(whole.width == 1920 && whole.height == 1080,
              "1x1 is the whole picture");
        CHECK(whole.plane[0] == f.plane[0], "starting at the top-left");

        const DecodedVideoFrame oor = tile_view(f, TileLayout::parse("2x1"), 9);
        CHECK(oor.width == 1920 && oor.height == 1080,
              "an out-of-range tile is the whole picture");
        CHECK(oor.plane[0] == f.plane[0], "and starts at the top-left");
    }

    if (g_fail) { std::printf("\n%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("\nall tile-crop checks passed\n");
    return 0;
}
