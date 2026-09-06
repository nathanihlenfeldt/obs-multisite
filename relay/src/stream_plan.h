#pragma once
//
// stream_plan.h — the single decision point: given what the main site is
// actually publishing and what the operator chose, either produce the exact
// ffmpeg invocation, or say in plain words why this cannot be sent.
//
// This is deliberately ONE pure function with no I/O. The UI calls it to warn
// an operator before they press Start; the relay calls it again before it
// spawns anything. If those two ever disagreed, the UI would promise something
// the relay then refuses — so they share this code rather than each having
// their own idea of what is possible.
//
// Why this refuses rather than adapts: ffmpeg will happily mux HEVC into FLV
// and exit 0 (enhanced RTMP), producing a well-formed stream that the
// destination then rejects. Measured, not assumed. So a codec that a
// destination cannot take has to be caught HERE, before anything is spawned —
// there is no error downstream to catch.
//
#include "destination.h"
#include "model.h"

#include <string>
#include <vector>

namespace multisite_relay {

struct StreamPlan {
    bool ok = false;

    // Why not, in the words a volunteer should read. Empty when ok.
    std::string problem;
    // What the operator could do about it, when there is something. Kept
    // separate from `problem` so the UI can style it as guidance.
    std::string remedy;

    // The full ffmpeg argument vector, argv[0] included. Only when ok.
    std::vector<std::string> args;

    // What was actually selected, for the log line and the status panel.
    // "Sending: 1920x1080 H.264, audio 'Sermon ISO' (stereo)". Sending a mic
    // ISO to the public stream by accident is the failure this exists to make
    // impossible to do silently.
    std::string summary;

    std::string audio_label;
    int         audio_index = -1;
};

// `input` is what ffmpeg reads. In the relay this is always "pipe:0": the
// feeder owns the write end and hands over one fragment at a time, and a pipe
// that goes quiet is what the machine reads as a stall.
StreamPlan plan_stream(const multisite::Manifest& manifest,
                       const Destination& dest,
                       const std::string& input);

// The destination URL with the stream key appended, which is what ffmpeg
// takes as its output. Exposed for testing that the key never lands anywhere
// it should not; callers should prefer plan_stream().
std::string output_url(const Destination& d);

// Everything in `args` except anything carrying the stream key, for logging.
// A stream key in a log file is a stream key on someone's pastebin.
std::vector<std::string> redact(const std::vector<std::string>& args,
                                const Destination& d);

} // namespace multisite_relay
