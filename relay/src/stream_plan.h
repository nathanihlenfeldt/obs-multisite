// SPDX-License-Identifier: GPL-3.0-or-later
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
// What may be sent depends on the protocol, which is why the two live in one
// function rather than two. RTMP means FLV, and FLV means H.264. SRT means
// MPEG-TS, which carries HEVC properly — a standardised stream type that
// decoders have handled for a decade, not FLV's after-the-fact extension that
// half the receiving end has never heard of. So an HEVC feed that cannot go to
// YouTube can go to an SRT destination, and the refusal is written per
// protocol instead of once for everything.
//
// AV1 is still refused on both. MPEG-TS has a mapping for it, but ffmpeg's
// support and the receiving end's support are each patchy enough that the
// likely outcome is the same well-formed-but-rejected stream this whole file
// exists to prevent.
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

// The full output URL as ffmpeg receives it: for RTMP the address with the
// stream key appended as a path component, and for SRT the address with the
// stream id, passphrase, latency and mode appended as query parameters.
// Exposed for testing that the secrets never land anywhere they should not;
// callers should prefer plan_stream().
std::string output_url(const Destination& d);

// Everything in `args` with the secrets taken out, for logging. A stream key
// in a log file is a stream key on someone's pastebin — and under SRT the
// secrets are buried inside a URL rather than sitting in an argument of their
// own, so this scrubs within each argument rather than dropping whole ones.
std::vector<std::string> redact(const std::vector<std::string>& args,
                                const Destination& d);

} // namespace multisite_relay
