#pragma once
//
// preview.h — a picture of the incoming feed in the operator's browser,
// deliberately NOT the picture going out.
//
// An operator lining up a cue needs to see what is coming while the screen in
// the room holds the last frame, or shows black. If the preview were the
// output there would be no way to look ahead without putting it to air, which
// is exactly the thing a satellite campus most needs to avoid.
//
// So: the newest decoded frame is copied aside on the delivery thread, and
// this turns it into a JPEG when a browser asks for one. The rate is the
// browser's choice — one a second is enough to line something up, and costs
// the box almost nothing.
//
#include "../core/cmaf_decoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multisite_player {

class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();

    // Scale to at most `max_width` (keeping the shape of the picture) and
    // encode. Returns false with `error` set if FFmpeg refused; the encoder
    // stays usable.
    bool encode(const multisite::DecodedVideoFrame& frame, int max_width,
                int quality, std::vector<uint8_t>& out, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite_player
