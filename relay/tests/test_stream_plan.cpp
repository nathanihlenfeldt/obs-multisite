// test_stream_plan.cpp — what may be sent onward, and what must be refused.
//
// The failure this guards against is not a crash. It is a relay that looks
// healthy while putting a mic ISO, a click track, or a codec the destination
// cannot decode out to the public. Every refusal here is a stream that would
// otherwise have gone out wrong with no error anywhere.
#include "../src/stream_plan.h"

#include <cstdio>
#include <string>

using namespace multisite_relay;
using multisite::AudioTrack;
using multisite::Manifest;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static AudioTrack track(int idx, const std::string& label, int ch = 2) {
    AudioTrack t;
    t.idx = idx; t.label = label; t.codec = "aac";
    t.channels = ch; t.sample_rate = 48000;
    return t;
}

// The ordinary multi-track service: a stereo main mix, a mono sermon ISO and
// a mono click, exactly as PROJECT-SCOPE.md 4.3 describes.
static Manifest ordinary() {
    Manifest m;
    m.video.codec = "h264"; m.video.width = 1920; m.video.height = 1080;
    m.audio_tracks = { track(0, "Main Mix", 2),
                       track(1, "Sermon ISO", 1),
                       track(2, "Click", 1) };
    return m;
}

static Destination dest(const std::string& label = "") {
    Destination d;
    d.name = "YouTube";
    d.room_id = "main-auditorium";
    d.url = "rtmp://a.rtmp.youtube.com/live2";
    d.stream_key = "secret-key-1234";
    d.audio.label = label;
    return d;
}

static bool has_pair(const std::vector<std::string>& a,
                     const std::string& k, const std::string& v) {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == k && a[i + 1] == v) return true;
    return false;
}

int main() {
    std::printf("stream plan\n");

    // ── The ordinary case ────────────────────────────────────────────────────
    {
        auto p = plan_stream(ordinary(), dest("Sermon ISO"), "/tmp/f.fifo");
        CHECK(p.ok, "a normal H.264 multi-track service can be sent");
        CHECK(p.audio_index == 1, "the chosen track resolves to its position");
        CHECK(has_pair(p.args, "-map", "0:a:1"), "ffmpeg is told to take a:1");
        CHECK(has_pair(p.args, "-map", "0:v:0"), "and the video");
        CHECK(has_pair(p.args, "-c", "copy"), "copy remux, no re-encoding");
        CHECK(p.summary.find("Sermon ISO") != std::string::npos,
              "the summary names what was actually sent");
    }

    // No choice made: the main mix, so a stereo-only church configures nothing.
    {
        auto p = plan_stream(ordinary(), dest(), "/tmp/f.fifo");
        CHECK(p.ok && p.audio_index == 0,
              "choosing nothing sends the first track");
    }

    // ── Refusals ─────────────────────────────────────────────────────────────
    {
        Manifest m = ordinary();
        m.video.codec = "hevc";
        auto p = plan_stream(m, dest("Main Mix"), "/tmp/f.fifo");
        CHECK(!p.ok, "HEVC is refused rather than muxed into FLV");
        CHECK(p.problem.find("H.264") != std::string::npos,
              "and the reason says what is needed");
        CHECK(p.problem.find("codec") == std::string::npos,
              "without using the word codec at a volunteer");
    }
    {
        Manifest m = ordinary();
        m.video.codec = "av1";
        CHECK(!plan_stream(m, dest("Main Mix"), "/tmp/f.fifo").ok,
              "so is AV1");
    }
    {
        // Packed multi-channel: mix, ISOs and click in one 8-channel stream.
        Manifest m;
        m.video.codec = "h264";
        AudioTrack packed = track(0, "Production", 8);
        packed.channel_labels = { "Main L", "Main R", "Sermon", "Click",
                                  "Spare", "Spare", "Spare", "Spare" };
        m.audio_tracks = { packed };
        auto p = plan_stream(m, dest(), "/tmp/f.fifo");
        CHECK(!p.ok, "a packed multi-channel feed is refused, not downmixed");
        CHECK(p.problem.find("click") != std::string::npos,
              "and says what is inside it, because that is the danger");
    }
    {
        Manifest m = ordinary();
        auto p = plan_stream(m, dest("Sermon Mic"), "/tmp/f.fifo");
        CHECK(!p.ok, "a saved track name that no longer exists is refused");
        CHECK(p.remedy.find("Main Mix") != std::string::npos,
              "and the operator is told what there is instead");
    }
    {
        // The dangerous one: published positions disagreeing with the file.
        Manifest m = ordinary();
        m.audio_tracks[1].idx = 2;
        auto p = plan_stream(m, dest("Sermon ISO"), "/tmp/f.fifo");
        CHECK(!p.ok, "a manifest whose track positions do not line up is refused");
    }
    {
        Manifest m = ordinary();
        m.audio_tracks.clear();
        CHECK(!plan_stream(m, dest(), "/tmp/f.fifo").ok, "no audio is refused");
    }
    {
        Destination d = dest("Main Mix");
        d.allow_transcode = true;
        CHECK(!plan_stream(ordinary(), d, "/tmp/f.fifo").ok,
              "asking to re-encode is refused, not quietly ignored");
    }

    // ── The stream key ───────────────────────────────────────────────────────
    {
        Destination d = dest("Main Mix");
        auto p = plan_stream(ordinary(), d, "/tmp/f.fifo");
        bool key_present = false;
        for (const auto& a : p.args)
            if (a.find("secret-key-1234") != std::string::npos) key_present = true;
        CHECK(key_present, "the key does reach ffmpeg");

        auto safe = redact(p.args, d);
        bool leaked = false;
        for (const auto& a : safe)
            if (a.find("secret-key-1234") != std::string::npos) leaked = true;
        CHECK(!leaked, "but never survives redaction for the log");
        CHECK(validate(d).empty(), "a complete destination validates");
    }
    {
        Destination d = dest();
        d.stream_key.clear();
        CHECK(!validate(d).empty(), "a missing stream key is caught when saving");
        d = dest(); d.url = "https://youtube.com/watch";
        CHECK(!validate(d).empty(), "so is a web address pasted in by mistake");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL STREAM PLAN TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
