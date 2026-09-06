#pragma once
//
// destination.h — one place a service is sent to, and what the operator chose
// for it.
//
// A destination is deliberately dumb data: a URL, a key, and a selection. All
// the judgement about whether it CAN be served (codec, audio shape) lives in
// compat.h, so the same checks run in the UI before an operator presses Start
// and again in the relay before ffmpeg is spawned. Those must never disagree.
//
#include <cstdint>
#include <string>

namespace multisite_relay {

// How the operator picked the audio. Selecting by LABEL is what the UI does —
// "Main mix", "Sermon ISO" — because a volunteer must never be asked for a
// track number. The index is kept as a fallback for a feed whose labels the
// encoder operator never set, and as what actually reaches ffmpeg.
struct AudioSelection {
    // Preferred: match the published label from the manifest. Empty means
    // "whatever is first", which is the right default for a stereo-only church
    // that has never thought about tracks.
    std::string label;
    // Resolved at start time from the label. -1 until then.
    int         resolved_index = -1;
};

struct Destination {
    int64_t     id = 0;
    std::string name;              // "YouTube", "Facebook" — operator's words
    std::string room_id;           // which feed this sends
    std::string url;               // rtmp://a.rtmp.youtube.com/live2
    std::string stream_key;        // secret; never logged, never sent to the UI
    AudioSelection audio;

    // Explicit, per the brief: a destination that needs re-encoding must be
    // switched on deliberately by an operator who has been told the cost.
    // Stage 1 has no encoder at all, so this being true is itself refused.
    bool        allow_transcode = false;

    // Whether the operator has asked for this to be running. Distinct from
    // whether it IS running: a destination can be enabled and waiting for the
    // encoder to go live.
    bool        enabled = false;

    // How far behind the live edge to sit, in seconds. This is the buffer that
    // absorbs a dropout at the main site so it never reaches the public
    // stream. 0 means "use the room default".
    int         delay_s = 0;
};

// Reasons a destination cannot be saved at all. Returns an empty string when
// it is fine. Phrased for the person reading it in the browser.
std::string validate(const Destination& d);

// Whether the difference between two versions of a destination is one that
// forces the stream to be rebuilt. Renaming it is not; changing where it goes,
// which sound it carries, or how far behind it sits, is.
//
// This distinction is the whole reason it exists: a destination that is on air
// must not be interrupted because something unrelated to it was edited.
bool affects_stream(const Destination& a, const Destination& b);

} // namespace multisite_relay
