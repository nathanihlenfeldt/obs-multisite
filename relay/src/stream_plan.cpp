#include "stream_plan.h"

#include <algorithm>

namespace multisite_relay {

using multisite::AudioTrack;
using multisite::Manifest;

namespace {

std::string channels_word(int ch) {
    if (ch == 1) return "mono";
    if (ch == 2) return "stereo";
    return std::to_string(ch) + " channels";
}

std::string join_labels(const std::vector<AudioTrack>& tracks) {
    std::string out;
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (i) out += (i + 1 == tracks.size()) ? " or " : ", ";
        out += "\"" + tracks[i].label + "\"";
    }
    return out;
}

// A packed multi-channel feed is one stream carrying the main mix, the ISOs
// and the click in fixed channel positions (PROJECT-SCOPE.md 4.3.1). It is
// identified by published channel labels, not by a mode flag — there isn't
// one — and only ever set beyond stereo.
bool is_packed(const AudioTrack& t) {
    return !t.channel_labels.empty() || t.channels > 2;
}

} // namespace

std::string output_url(const Destination& d) {
    if (d.stream_key.empty()) return d.url;
    std::string u = d.url;
    if (!u.empty() && u.back() == '/') u.pop_back();
    return u + "/" + d.stream_key;
}

std::vector<std::string> redact(const std::vector<std::string>& args,
                                const Destination& d) {
    std::vector<std::string> out;
    out.reserve(args.size());
    const std::string full = output_url(d);
    for (const auto& a : args) {
        if (!d.stream_key.empty() && a == full) {
            std::string u = d.url;
            if (!u.empty() && u.back() == '/') u.pop_back();
            out.push_back(u + "/<key>");
        } else if (!d.stream_key.empty() &&
                   a.find(d.stream_key) != std::string::npos) {
            out.push_back("<redacted>");
        } else {
            out.push_back(a);
        }
    }
    return out;
}

StreamPlan plan_stream(const Manifest& manifest,
                       const Destination& dest,
                       const std::string& input) {
    StreamPlan p;

    // ── Video ────────────────────────────────────────────────────────────────
    // RTMP is H.264 in practice. ffmpeg will mux HEVC or AV1 into FLV without
    // complaint, so this check is the only thing standing between an HEVC feed
    // and a stream that looks healthy here and is dead at the destination.
    std::string vc = manifest.video.codec;
    std::transform(vc.begin(), vc.end(), vc.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (vc.empty()) {
        p.problem = "The main site has not said what kind of video this is, "
                    "so it cannot be sent on safely.";
        p.remedy  = "Check that the broadcast is running a current version of "
                    "the encoder.";
        return p;
    }
    if (vc != "h264") {
        p.problem = "This service is being recorded as " + vc +
                    " video, and streaming sites need H.264.";
        p.remedy  = "The video would have to be re-encoded on the way out, "
                    "which this server cannot do yet. Set the main site's "
                    "encoder to H.264 for services you want to stream "
                    "publicly.";
        return p;
    }
    if (dest.allow_transcode) {
        // Refused rather than ignored: silently treating "re-encode this" as
        // "copy it" is exactly the quiet wrong answer the brief rules out.
        p.problem = "This destination is set to re-encode the video.";
        p.remedy  = "Re-encoding is not built yet. Turn it off to send the "
                    "service as-is.";
        return p;
    }

    // ── Audio ────────────────────────────────────────────────────────────────
    const auto& tracks = manifest.audio_tracks;
    if (tracks.empty()) {
        p.problem = "This service has no sound in it.";
        p.remedy  = "Check that the main site has at least one audio track "
                    "switched on.";
        return p;
    }

    // Packed multi-channel: one stream carrying the mix, the ISOs and the
    // click together. Sending it on unchanged would put a mic ISO or the click
    // track out to the public. Selecting a channel pair out of it is Stage 2;
    // until then this is refused rather than guessed at.
    for (const auto& t : tracks) {
        if (!is_packed(t)) continue;
        p.problem = "The main site is sending its sound as one "
                    + channels_word(t.channels) +
                    " track with the mix, the microphones and the click all "
                    "inside it.";
        p.remedy  = "Picking one pair out of that is not built yet. Set the "
                    "main site to send separate audio tracks instead.";
        return p;
    }

    // Resolve the operator's choice. By label, always — the UI never shows an
    // index, and a saved destination must keep meaning the same thing even if
    // the encoder operator reorders their tracks between services.
    int index = -1;
    std::string label;
    if (dest.audio.label.empty()) {
        index = 0;                       // the main mix, by convention
        label = tracks[0].label;
    } else {
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].label != dest.audio.label) continue;
            index = (int)i;
            label = tracks[i].label;
            break;
        }
        if (index < 0) {
            p.problem = "This service does not have a sound feed called \"" +
                        dest.audio.label + "\".";
            p.remedy  = "It is sending " + join_labels(tracks) +
                        ". Choose one of those instead.";
            return p;
        }
    }

    const AudioTrack& chosen = tracks[(size_t)index];

    // The manifest publishes each track's position among the audio streams as
    // `idx`. ffmpeg's -map 0:a:N counts the same way, so the two agree — but
    // only if the encoder wrote them in order. A feed where they disagree
    // would send the wrong track while looking perfectly correct, so it is
    // refused rather than trusted.
    if (chosen.idx != index) {
        p.problem = "The main site's description of its sound feeds does not "
                    "line up with the recording itself.";
        p.remedy  = "Sending it could put the wrong microphone on air, so it "
                    "has been stopped. Please report this.";
        return p;
    }

    std::string ac = chosen.codec;
    std::transform(ac.begin(), ac.end(), ac.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ac != "aac") {
        p.problem = "The sound feed \"" + label + "\" is " + ac +
                    ", and streaming sites need AAC.";
        p.remedy  = "Set the main site's encoder to AAC audio.";
        return p;
    }

    // ── The invocation ───────────────────────────────────────────────────────
    // Copy remux only: no decode, no encode, no quality loss, almost no CPU.
    p.args = {
        "ffmpeg",
        "-hide_banner",
        // The feeder paces the pipe, so ffmpeg must not add pacing of its own.
        // Measured in the Stage 0 rig: a pipe fed one fragment per segment
        // holds a steady 1.0x with no -re, because being starved between
        // writes IS the clock.
        "-i", input,
        "-map", "0:v:0",
        "-map", "0:a:" + std::to_string(index),
        "-c", "copy",
        // FLV cannot rewrite its header over a socket; without this ffmpeg
        // logs two alarming failures per run that mean nothing.
        "-flvflags", "no_duration_filesize",
        "-f", "flv",
        output_url(dest),
    };

    p.ok = true;
    p.audio_index = index;
    p.audio_label = label;

    std::string res;
    if (manifest.video.width > 0 && manifest.video.height > 0) {
        res = std::to_string(manifest.video.width) + "x" +
              std::to_string(manifest.video.height) + " ";
    }
    p.summary = "sending " + res + "H.264 video with the \"" + label +
                "\" sound feed (" + channels_word(chosen.channels) + ")";
    return p;
}

} // namespace multisite_relay
