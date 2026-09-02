#pragma once
//
// cmaf_decoder.h — decodes a CMAF stream that arrives as an init segment
// followed by media fragments.
//
// FFmpeg wants a continuous byte stream, but we receive discrete objects. This
// wraps a blocking byte queue behind a custom AVIO read callback: push the init
// segment, then push fragments as they're cached, and the decoder consumes them
// as if reading one long file.
//
// Deliberately free of any OBS dependency so it can be tested against real
// captured segments.
//
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace multisite {

struct DecodedVideoFrame {
    int width = 0, height = 0;
    // I420 planes (Y, U, V) with their strides.
    std::vector<uint8_t> data;
    uint8_t* plane[3] = { nullptr, nullptr, nullptr };
    int      stride[3] = { 0, 0, 0 };
    int64_t  pts_ns = 0;          // presentation time within the stream
    bool     full_range = false;
};

struct DecodedAudioFrame {
    int      sample_rate = 48000;
    int      channels = 2;
    int      track_index = 0;     // which audio track this came from
    uint32_t frames = 0;
    std::vector<float> interleaved;   // planar-free, ready for OBS
    int64_t  pts_ns = 0;
};

using VideoFrameCallback = std::function<void(const DecodedVideoFrame&)>;
using AudioFrameCallback = std::function<void(const DecodedAudioFrame&)>;

class CmafDecoder {
public:
    CmafDecoder();
    ~CmafDecoder();

    void on_video(VideoFrameCallback cb);
    void on_audio(AudioFrameCallback cb);

    // Push the init segment. Must be called before any fragment. Starts the
    // decode thread, which opens the stream and runs until stop().
    bool start(const std::vector<uint8_t>& init_segment);

    // Queue a media fragment for decoding. Non-blocking unless the internal
    // buffer is full (which back-pressures the caller rather than ballooning).
    void push_fragment(const std::vector<uint8_t>& bytes);

    // Bytes currently queued but not yet consumed by the decoder.
    size_t queued_bytes() const;

    // Signal end of stream and wait for the decode thread to finish.
    void stop();

    bool ok() const;
    const std::string& error() const;

    // Stream properties, valid once the init segment has been parsed.
    int  video_width() const;
    int  video_height() const;
    int  audio_track_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace multisite
