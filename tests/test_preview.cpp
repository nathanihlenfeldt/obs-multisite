// SPDX-License-Identifier: GPL-3.0-or-later
// test_preview.cpp — the preview's JPEG encoder must be reusable.
//
// The preview re-encodes the same decoded frame to JPEG once per request. If
// the encoder could only ever produce one frame, the browser would show that
// frame and then go blank — which is exactly the bug this guards against. The
// test builds a real I420 frame, encodes it twice through the same JpegEncoder,
// and checks that both outputs are valid JPEGs.
#include "preview.h"
#include "cmaf_decoder.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace multisite;
using namespace multisite_player;

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while (0)

// A solid grey I420 frame with planes/stride laid out the way the decoder's
// DecodedVideoFrame carries them (Y, then U, then V).
static DecodedVideoFrame make_frame(int w, int h) {
    DecodedVideoFrame f;
    f.width  = w;
    f.height = h;
    f.stride[0] = w;
    f.stride[1] = w / 2;
    f.stride[2] = w / 2;
    const int y_size  = w * h;
    const int uv_size = (w / 2) * (h / 2);
    f.data.resize((size_t)(y_size + 2 * uv_size));
    std::memset(f.data.data(), 128, (size_t)y_size);                // luma
    std::memset(f.data.data() + y_size, 128, (size_t)(2 * uv_size)); // chroma
    f.plane[0] = f.data.data();
    f.plane[1] = f.data.data() + y_size;
    f.plane[2] = f.data.data() + y_size + uv_size;
    return f;
}

static bool is_jpeg(const std::vector<uint8_t>& b) {
    return b.size() > 4 && b[0] == 0xFF && b[1] == 0xD8 &&
           b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9;
}

int main() {
    const DecodedVideoFrame frame = make_frame(320, 180);

    JpegEncoder enc;
    std::vector<uint8_t> a, b;
    std::string err;

    std::printf("== Preview encoder is reusable across requests ==\n");
    CHECK(enc.encode(frame, 320, 70, a, err), "first encode succeeds");
    CHECK(is_jpeg(a), "first output is a JPEG (SOI/EOI markers present)");
    CHECK(enc.encode(frame, 320, 70, b, err), "second encode succeeds");
    CHECK(is_jpeg(b), "second output is a JPEG (SOI/EOI markers present)");
    CHECK(a.size() > 0 && b.size() > 0, "both outputs are non-empty");
    std::printf("     (JPEG sizes: %zu, %zu bytes)\n", a.size(), b.size());

    // Scaling is a separate code path (a different sws context), so prove that
    // works repeatedly too — a phone asks for a small preview, not the full
    // frame.
    CHECK(enc.encode(frame, 160, 70, a, err), "downscaled encode succeeds");
    CHECK(is_jpeg(a), "downscaled output is a JPEG");

    std::printf("\n%s\n", g_fail == 0 ? "ALL PREVIEW TESTS PASSED"
                                       : "SOME PREVIEW TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
