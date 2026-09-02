// test_cmaf_decode.cpp — decodes a CMAF stream through CmafDecoder.
//
// Runs against REAL captured output when available (test-data/real_init.mp4 +
// real_seg.m4s, taken straight from the R2 bucket), otherwise falls back to a
// generated fixture. This is the decoder half of the round trip: the same
// bytes the encoder produced are decoded back into frames.
#include "../src/core/cmaf_decoder.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static std::vector<uint8_t> readfile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

int main(int argc, char** argv) {
    std::string init_path = argc > 1 ? argv[1] : "test-data/real_init.mp4";
    std::string seg_path  = argc > 2 ? argv[2] : "test-data/real_seg.m4s";

    auto init = readfile(init_path);
    auto seg  = readfile(seg_path);
    if (init.empty() || seg.empty()) {
        std::printf("  [skip] no fixture at %s / %s\n",
                    init_path.c_str(), seg_path.c_str());
        return 0;
    }
    std::printf("fixture: init=%zu bytes, segment=%zu bytes\n",
                init.size(), seg.size());

    std::atomic<int> vframes{0};
    std::atomic<int> aframes{0};
    std::atomic<int> vwidth{0}, vheight{0};
    std::atomic<long long> last_v_pts{-1};
    std::atomic<bool> pts_monotonic{true};
    std::atomic<bool> planes_valid{true};

    CmafDecoder dec;
    dec.on_video([&](const DecodedVideoFrame& f) {
        ++vframes;
        vwidth = f.width; vheight = f.height;
        // Planes must be populated and strides sane, or OBS would read garbage.
        if (!f.plane[0] || !f.plane[1] || !f.plane[2] ||
            f.stride[0] < f.width || f.data.empty())
            planes_valid = false;
        long long prev = last_v_pts.load();
        if (prev >= 0 && f.pts_ns < prev) pts_monotonic = false;
        last_v_pts = f.pts_ns;
    });
    dec.on_audio([&](const DecodedAudioFrame& f) {
        ++aframes;
        if (f.interleaved.size() != (size_t)f.frames * f.channels)
            planes_valid = false;
    });

    CHECK(dec.start(init), "decoder started with the init segment");
    dec.push_fragment(seg);

    // Give the decode thread time to drain the queue.
    for (int i = 0; i < 200 && dec.queued_bytes() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    dec.stop();

    CHECK(dec.ok(), dec.ok() ? "decoder reported no error" : dec.error().c_str());
    CHECK(vframes.load() > 0, "decoded video frames from the fragment");
    CHECK(vwidth.load() > 0 && vheight.load() > 0, "video dimensions reported");
    CHECK(planes_valid.load(), "frame planes and strides are valid");
    CHECK(pts_monotonic.load(), "video timestamps are non-decreasing");
    std::printf("     (decoded %d video frames at %dx%d, %d audio frames)\n",
                vframes.load(), vwidth.load(), vheight.load(), aframes.load());
    CHECK(aframes.load() > 0, "decoded audio frames");

    std::printf("\n%s\n", g_fail == 0 ? "CMAF DECODE TESTS PASSED"
                                      : "CMAF DECODE TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
