// SPDX-License-Identifier: GPL-3.0-or-later
// test_tile_layout.cpp — dividing one encoded picture into discrete ones.
//
// The geometry is worth pinning because every way of getting it wrong is quiet.
// A rectangle one pixel out still shows a picture. An odd offset still shows a
// picture, with the colour sheared off the luma. A layout parsed from a string
// nobody validated still shows a picture, of the wrong half of the programme.
// None of that announces itself on a screen at the back of a church.
#include "../src/core/model.h"

#include <cstdio>
#include <string>

using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static bool rect_is(const TileLayout::Rect& r, int x, int y, int w, int h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

int main() {
    std::printf("== an absent or unreadable layout is one picture ==\n");
    {
        // Every event written before this existed has no layout field, and must
        // keep working untouched.
        CHECK(TileLayout::parse("").count() == 1, "empty string is 1x1");
        CHECK(TileLayout::parse("1x1").count() == 1, "1x1 is 1x1");
        CHECK(!TileLayout::parse("1x1").is_split(), "and is not a split");
        // Strictness: an unrecognised layout must fall back to the whole
        // picture, which an operator can crop by hand, rather than to a guess
        // that looks deliberate and is wrong.
        const char* junk[] = { "2", "x2", "2x", "axb", "2x2x2", "-2x2",
                               "2 x 2", "2x2 ", "999x999", "0x2", "2x0" };
        bool all_one = true;
        for (const char* s : junk)
            if (TileLayout::parse(s).count() != 1) { all_one = false;
                std::printf("        (accepted %s)\n", s); }
        CHECK(all_one, "junk, negatives, blanks and huge counts all fall back");
    }

    std::printf("== a layout round-trips through its string ==\n");
    {
        for (const char* s : { "2x1", "1x2", "2x2", "3x3", "4x4" }) {
            const TileLayout t = TileLayout::parse(s);
            if (t.to_string() != s) {
                std::printf("  [FAIL] %s round-tripped to %s\n", s,
                            t.to_string().c_str());
                ++g_fail;
            }
        }
        std::printf("  [ok]   every accepted layout survives the round trip\n");
    }

    std::printf("== 2x1 splits a 3840x1080 feed down the middle ==\n");
    {
        // The case the roadmap names: two 1920x1080 pictures sent as one wide
        // frame. Note this is exactly why the layout is published rather than
        // inferred — this frame is indistinguishable from a real ultrawide.
        const TileLayout t = TileLayout::parse("2x1");
        CHECK(t.count() == 2, "two tiles");
        CHECK(rect_is(t.tile_rect(0, 3840, 1080), 0, 0, 1920, 1080),
              "tile 0 is the left half");
        CHECK(rect_is(t.tile_rect(1, 3840, 1080), 1920, 0, 1920, 1080),
              "tile 1 is the right half");
    }

    std::printf("== 2x2 numbers tiles in reading order ==\n");
    {
        const TileLayout t = TileLayout::parse("2x2");
        CHECK(t.count() == 4, "four tiles");
        CHECK(rect_is(t.tile_rect(0, 1920, 1080), 0, 0, 960, 540),
              "tile 0 top-left");
        CHECK(rect_is(t.tile_rect(1, 1920, 1080), 960, 0, 960, 540),
              "tile 1 top-right");
        CHECK(rect_is(t.tile_rect(2, 1920, 1080), 0, 540, 960, 540),
              "tile 2 bottom-left");
        CHECK(rect_is(t.tile_rect(3, 1920, 1080), 960, 540, 960, 540),
              "tile 3 bottom-right");
    }

    std::printf("== every edge is even, for I420's half-resolution chroma ==\n");
    {
        // An odd offset or width has no chroma sample to start from, and the
        // crop shears the colour away from the luma — a picture that is subtly,
        // permanently wrong rather than obviously broken.
        const int sizes[][2] = { {1920,1080}, {1921,1081}, {1279,721},
                                 {3840,2161}, {999,999} };
        bool all_even = true;
        for (const char* s : { "2x1", "1x2", "2x2", "3x3" }) {
            const TileLayout t = TileLayout::parse(s);
            for (const auto& sz : sizes)
                for (int i = 0; i < t.count(); ++i) {
                    const auto r = t.tile_rect(i, sz[0], sz[1]);
                    if ((r.x | r.y | r.w | r.h) & 1) { all_even = false;
                        std::printf("        (%s tile %d of %dx%d -> %d,%d %dx%d)\n",
                                    s, i, sz[0], sz[1], r.x, r.y, r.w, r.h); }
                }
        }
        CHECK(all_even, "no odd offset or size at any layout or frame size");
    }

    std::printf("== tiles stay inside the frame and do not overlap ==\n");
    {
        const TileLayout t = TileLayout::parse("2x2");
        const int W = 1921, H = 1081;      // deliberately odd
        bool inside = true;
        for (int i = 0; i < t.count(); ++i) {
            const auto r = t.tile_rect(i, W, H);
            if (r.x < 0 || r.y < 0 || r.x + r.w > W || r.y + r.h > H)
                inside = false;
        }
        CHECK(inside, "an odd frame size still yields rectangles within it");

        const auto a = t.tile_rect(0, 1920, 1080);
        const auto b = t.tile_rect(1, 1920, 1080);
        CHECK(a.x + a.w <= b.x, "neighbouring tiles do not overlap");
    }

    std::printf("== a tile the layout does not have is the whole picture ==\n");
    {
        // A source configured for tile 3 of a 2x1 event is misconfigured. A
        // whole picture says so on screen; a black rectangle looks like a
        // failure of something else entirely.
        const TileLayout t = TileLayout::parse("2x1");
        CHECK(rect_is(t.tile_rect(7, 1920, 1080), 0, 0, 1920, 1080),
              "out of range gives the whole frame");
        CHECK(rect_is(t.tile_rect(-1, 1920, 1080), 0, 0, 1920, 1080),
              "so does a negative index");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL TILE LAYOUT TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
