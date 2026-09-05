#pragma once
//
// splash.h — what a campus screen shows when there is no programme on it.
//
// The first problem with an appliance is finding it. A box is plugged in at
// the back of a hall, and until somebody knows its address there is no way to
// configure it, no way to see whether it is working, and nothing to type into
// a phone. So the very first thing it does with the display it has just
// claimed is put its own address on it, in letters readable from the far side
// of a room.
//
// The text is drawn from a 5x7 bitmap font defined in the source rather than
// loaded from a font file. It is not beautiful, but it needs no font package,
// no library, and cannot fail at three o'clock on a Sunday because a
// dependency moved.
//
#include <cstdint>
#include <string>
#include <vector>

namespace multisite_player {

// Packed BGRX, 4 bytes per pixel — what every KMS driver will scan out and
// what the display path already expects.
class Canvas {
public:
    Canvas(int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }
    int stride() const { return m_width * 4; }
    const uint8_t* pixels() const {
        return reinterpret_cast<const uint8_t*>(m_px.data());
    }

    void fill(uint32_t bgrx);
    void rect(int x, int y, int w, int h, uint32_t bgrx);

    // Draws in capitals; the font has no lowercase, which reads as deliberate
    // on a title card and saves half the glyph table.
    void text(int x, int y, const std::string& s, int scale, uint32_t bgrx);
    // Width in pixels the same call would occupy, for centring.
    static int text_width(const std::string& s, int scale);
    static int text_height(int scale) { return 7 * scale; }
    void text_centred(int y, const std::string& s, int scale, uint32_t bgrx);

private:
    int m_width, m_height;
    std::vector<uint32_t> m_px;
};

// What the splash has to say. Everything here is something an operator
// standing in front of the screen would otherwise have to find a keyboard to
// discover.
struct SplashInfo {
    std::string hostname;
    std::vector<std::string> addresses;   // "http://10.0.1.24:8080"
    std::string room;
    std::string state;                    // "WAITING FOR THE MAIN SITE"
    std::string detail;                   // a second line, when there is one
    std::string version;
    bool        configured = true;
};

void render_splash(Canvas& canvas, const SplashInfo& info);

// A still supplied by the campus, for the holding-slide idle mode. Decoded
// with FFmpeg, so it accepts whatever a church office is likely to produce.
// Returns false with `error` set rather than leaving a blank screen with no
// explanation.
bool load_still(const std::string& path, int width, int height, Canvas& out,
                std::string& error);

} // namespace multisite_player
