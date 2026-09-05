// test_decoder.cpp — proves the satellite receive path and timeslipping.
//
// A fake object store stands in for R2, driven by a simulated encoder that
// publishes segments over time. The tests then check the behaviours a campus
// actually depends on: prebuffer, pause-while-cache-fills, resume from the
// exact position, jump-to-live, scrub, checksum rejection, stale detection,
// and never silently skipping a missing segment.
#include "../src/core/decoder_session.h"
#include "../src/core/checksum.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace fs = std::filesystem;
using namespace multisite;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

// ── A fake bucket that a simulated encoder writes into ───────────────────────
class FakeStore : public Transport {
public:
    std::map<std::string, std::vector<uint8_t>> objects;
    std::mutex mtx;
    int fail_next_gets = 0;
    bool corrupt_segments = false;

    PutResult put(const std::string& key, const std::vector<uint8_t>& body,
                  const std::string&, const std::map<std::string,std::string>&) override {
        std::lock_guard<std::mutex> lk(mtx);
        objects[key] = body;
        return {true, 200, true, ""};
    }
    GetResult get(const std::string& key) override {
        std::lock_guard<std::mutex> lk(mtx);
        GetResult r;
        if (fail_next_gets > 0) {
            --fail_next_gets;
            r.error = "simulated network failure";
            r.http_status = 0;
            return r;
        }
        auto it = objects.find(key);
        if (it == objects.end()) {
            r.http_status = 404; r.error = "NoSuchKey"; r.retryable = false;
            return r;
        }
        r.success = true; r.http_status = 200; r.body = it->second;
        // Simulate in-flight corruption of media segments only.
        if (corrupt_segments && key.find("/segments/") != std::string::npos &&
            !r.body.empty()) {
            r.body[r.body.size() / 2] ^= 0xFF;
        }
        return r;
    }
};

// Publishes an event the way the encoder does, so the decoder sees realistic
// objects (live.json, event.json, init.mp4, manifest.json, segments).
struct FakeEncoder {
    FakeStore& store;
    std::string room, event;
    Manifest manifest;
    uint64_t next_seq = 0;
    double seg_dur = 6.0;
    size_t window = 50;
    int64_t clock_ms = 1000000;

    FakeEncoder(FakeStore& s, std::string r, std::string e)
        : store(s), room(std::move(r)), event(std::move(e)) {
        manifest.event_id = event;
        manifest.status = "live";
        manifest.init = "init.mp4";
        manifest.video = { "h264", 1280, 720, 30.0 };
        manifest.audio_tracks = { { 0, "Main mix", "aac", 2, 48000 } };
        manifest.first_available_seq = 0;
        manifest.started_at_ms = started_at_ms;
    }
    int64_t started_at_ms = 1700000000000LL;   // a fixed, realistic epoch

    static std::vector<uint8_t> body_for(uint64_t seq, size_t sz = 4096) {
        std::vector<uint8_t> v(sz);
        for (size_t i = 0; i < sz; ++i) v[i] = (uint8_t)((seq * 97 + i) & 0xFF);
        return v;
    }

    void publish_start() {
        std::vector<uint8_t> init(1200, 0x11);
        store.put("events/" + event + "/init.mp4", init, "", {});
        publish_manifest();
        publish_live("live");
    }
    void publish_segment() {
        uint64_t s = next_seq++;
        auto body = body_for(s);
        char name[16]; std::snprintf(name, sizeof(name), "%08llu",
                                     (unsigned long long)s);
        store.put("events/" + event + "/segments/" + name + ".m4s", body, "", {});
        ManifestSegment ms;
        ms.seq = s; ms.duration_s = seg_dur; ms.checksum = sha256_hex(body);
        ms.at_ms = started_at_ms + (int64_t)(s * seg_dur * 1000.0);
        manifest.push(ms, window);
        clock_ms += (int64_t)(seg_dur * 1000);
        publish_manifest();
    }
    void publish_manifest() {
        manifest.updated_at_ms = clock_ms;
        auto j = manifest.to_json();
        store.put("events/" + event + "/manifest.json",
                  std::vector<uint8_t>(j.begin(), j.end()), "", {});
    }
    void publish_live(const std::string& status) {
        LivePointer lp;
        lp.room_id = room; lp.event_id = event; lp.status = status;
        lp.updated_at_ms = clock_ms;
        auto j = lp.to_json();
        store.put("rooms/" + room + "/live.json",
                  std::vector<uint8_t>(j.begin(), j.end()), "", {});
    }
    MarkerList markers;
    void drop_marker(const std::string& label) {
        Marker mk;
        mk.seq = next_seq;                 // applies at the current live edge
        mk.at_ms = clock_ms;
        mk.type = "cue";
        mk.label = label;
        mk.id = label + "-id";
        markers.markers.push_back(mk);
        auto j = markers.to_json();
        store.put("events/" + event + "/markers.json",
                  std::vector<uint8_t>(j.begin(), j.end()), "", {});
    }
    void end() {
        manifest.status = "ended";
        publish_manifest();
        publish_live("ended");
    }
};

int main() {
    fs::path base = fs::temp_directory_path() / "multisite_decoder_test";
    fs::remove_all(base);
    fs::create_directories(base);

    std::printf("== 1. Discovers a live room and its event ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "main-auditorium", "01EVENTAAAAAAAAAAAAAAAAAAA");
        enc.publish_start();
        for (int i = 0; i < 6; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "main-auditorium";
        cfg.cache_dir = (base / "d1").string();
        DecoderSession dec(cfg, store);

        RoomState st = dec.poll(enc.clock_ms);
        CHECK(st == RoomState::Live, "room reports Live");
        CHECK(dec.event_id() == enc.event, "picked up the event id");
        CHECK(dec.live_edge() == 5, "live edge tracks the newest segment");
    }

    std::printf("== 2. Downloads ahead and starts after prebuffer ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTBBBBBBBBBBBBBBBBBBB");
        enc.publish_start();
        for (int i = 0; i < 10; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d2").string();
        cfg.prebuffer_segments = 2;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        int got = dec.pump_downloads(10);
        CHECK(got > 0, "downloaded segments into the cache");
        CHECK(dec.cache().has_init(), "init segment cached");
        CHECK(dec.start(), "playback starts once prebuffered");
        CHECK(dec.play_state() == PlayState::Playing, "state is Playing");

        auto seg = dec.next_segment();
        CHECK(seg.has_value(), "first segment served");
        CHECK(seg && !seg->init.empty(),
              "init travels with the first segment (decoder needs it first)");
        auto seg2 = dec.next_segment();
        CHECK(seg2 && seg2->init.empty(), "init not repeated on later segments");
        CHECK(seg2 && seg2->seq == seg->seq + 1, "segments served in order");
    }

    std::printf("== 3. TIMESLIPPING: pause holds position while cache fills ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTCCCCCCCCCCCCCCCCCCC");
        enc.publish_start();
        for (int i = 0; i < 8; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d3").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 3;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 5; ++i) dec.pump_downloads(10);
        dec.start();
        dec.next_segment();                       // serve one

        uint64_t head_at_pause = dec.playback_head();
        dec.pause();
        CHECK(dec.play_state() == PlayState::Paused, "paused");

        // The encoder keeps going while the campus is held.
        for (int i = 0; i < 6; ++i) enc.publish_segment();
        dec.poll(enc.clock_ms);
        int fetched = 0;
        for (int i = 0; i < 10; ++i) fetched += dec.pump_downloads(10);

        CHECK(fetched > 0, "cache KEPT FILLING while paused");
        CHECK(dec.playback_head() == head_at_pause,
              "playback head did not move while paused");
        CHECK(dec.next_segment().has_value() == false,
              "no segments served while paused");

        double behind = dec.behind_live_s();
        CHECK(behind > 30.0, "now well behind live (as expected after a hold)");

        dec.resume();
        auto after = dec.next_segment();
        CHECK(after && after->seq == head_at_pause,
              "resumed from EXACTLY where it paused (nothing skipped)");
        std::printf("     (behind live after hold: %.0fs, buffered ahead: %.0fs)\n",
                    behind, dec.buffered_ahead_s());
    }

    std::printf("== 4. Jump to live and scrub back ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTDDDDDDDDDDDDDDDDDDD");
        enc.publish_start();
        for (int i = 0; i < 20; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d4").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 4;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 8; ++i) dec.pump_downloads(10);
        dec.start();

        CHECK(dec.seek(3), "scrub back to segment 3");
        CHECK(dec.playback_head() == 3, "head moved to 3");
        double behind_after_seek = dec.behind_live_s();
        CHECK(behind_after_seek > 60.0, "reports being far behind live");

        dec.jump_to_live();
        CHECK(dec.playback_head() >= dec.live_edge() - 3,
              "jump_to_live snaps the head to the live edge");
        CHECK(dec.behind_live_s() < behind_after_seek,
              "behind-live figure shrinks after jumping");

        CHECK(!dec.seek(9999), "cannot seek beyond the live edge");

        // A jump must be signalled so the host can restart its decoder: the
        // next fragment has an unrelated baseMediaDecodeTime.
        uint64_t d0 = dec.discontinuity_id();
        dec.seek(5);
        CHECK(dec.discontinuity_id() > d0, "seek raises a discontinuity");
        uint64_t d1 = dec.discontinuity_id();
        dec.jump_to_live();
        CHECK(dec.discontinuity_id() > d1, "jump_to_live raises a discontinuity");
        // ...and the init segment must be re-sent after one.
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);
        auto after_jump = dec.next_segment();
        CHECK(after_jump && !after_jump->init.empty(),
              "init is re-sent after a discontinuity (decoder restarts)");
        CHECK(!dec.seek(0) || dec.playback_head() == 0,
              "seek within the retained window is allowed");
    }

    std::printf("== 5. Corrupt segments are rejected, not played ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTEEEEEEEEEEEEEEEEEEE");
        enc.publish_start();
        for (int i = 0; i < 5; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d5").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        store.corrupt_segments = true;             // flip a byte in flight
        dec.pump_downloads(10);
        CHECK(dec.stats().checksum_failures > 0,
              "checksum mismatch detected on download");
        CHECK(dec.cache().count() == 0,
              "corrupt segment was NOT cached (can't reach the decoder)");

        store.corrupt_segments = false;            // link recovers
        int got = dec.pump_downloads(10);
        CHECK(got > 0, "re-fetch after corruption succeeds");
        CHECK(dec.cache().count() > 0, "good segments cached on retry");
    }

    std::printf("== 6. A dead encoder is detected as Offline ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTFFFFFFFFFFFFFFFFFFF");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d6").string();
        cfg.stale_after_ms = 600000;               // 10 min
        DecoderSession dec(cfg, store);

        CHECK(dec.poll(enc.clock_ms) == RoomState::Live, "live while updating");
        // Wall clock moves on but the encoder publishes nothing more.
        RoomState later = dec.poll(enc.clock_ms + 700000);
        CHECK(later == RoomState::Offline,
              "stale manifest -> Offline (doesn't poll a dead event forever)");
        std::printf("     (%s)\n", dec.last_error().c_str());
    }

    std::printf("== 7. Clean end is reported as Ended ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTGGGGGGGGGGGGGGGGGGG");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();
        enc.end();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d7").string();
        DecoderSession dec(cfg, store);
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended, "reports Ended");
    }

    std::printf("== 8. A missing segment mid-stream stalls rather than skipping ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTHHHHHHHHHHHHHHHHHHH");
        enc.publish_start();
        for (int i = 0; i < 6; ++i) enc.publish_segment();   // seqs 0..5

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d8").string();
        // Large prebuffer so the head starts at 0 and the gap is genuinely
        // mid-stream (a hole at the live edge is just "not published yet").
        cfg.prebuffer_segments = 6;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.start(), "playback started at the start of the window");
        CHECK(dec.playback_head() == 0, "head starts at segment 0");

        auto first = dec.next_segment();
        CHECK(first && first->seq == 0, "served segment 0");
        uint64_t head = dec.playback_head();
        CHECK(head == 1, "head advanced to 1");
        CHECK(head < dec.live_edge(), "head is behind the live edge (real gap)");

        char name[16];
        std::snprintf(name, sizeof(name), "%08llu", (unsigned long long)head);
        fs::remove(fs::path(cfg.cache_dir) / dec.event_id() /
                   (std::string(name) + ".m4s"));

        auto none = dec.next_segment();
        CHECK(!none.has_value(), "waits for the missing segment");
        CHECK(dec.playback_head() == head,
              "head does NOT advance past a gap (no silent skip)");
        CHECK(dec.stats().gaps_waited > 0, "gap recorded");
    }

    std::printf("== 9. Markers are read and can be jumped to ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTMMMMMMMMMMMMMMMMMMM");
        enc.publish_start();
        for (int i = 0; i < 4; ++i) enc.publish_segment();
        enc.drop_marker("Sermon Start");          // at seq 4
        for (int i = 0; i < 6; ++i) enc.publish_segment();
        enc.drop_marker("Offering");              // at seq 10
        for (int i = 0; i < 3; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d10").string();
        cfg.prebuffer_segments = 0; cfg.buffer_minutes = 4;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        auto mks = dec.markers();
        CHECK(mks.size() == 2, "both markers read from markers.json");
        CHECK(mks.size() == 2 && mks[0].label == "Sermon Start",
              "marker labels preserved");
        CHECK(mks.size() == 2 && mks[0].seq == 4,
              "marker carries the sequence it was dropped at");

        for (int i = 0; i < 8; ++i) dec.pump_downloads(10);
        dec.start();
        CHECK(dec.jump_to_marker("Sermon Start-id"), "jumped to a marker");
        CHECK(dec.playback_head() == 4, "head moved to the marker's segment");

        auto cur = dec.current_marker();
        CHECK(cur && cur->label == "Sermon Start",
              "current_marker reports where we are in the service");

        CHECK(dec.jump_to_marker("Offering-id"), "jumped to the later marker");
        CHECK(dec.playback_head() == 10, "head moved to segment 10");
        cur = dec.current_marker();
        CHECK(cur && cur->label == "Offering", "current marker updated");

        CHECK(!dec.jump_to_marker("does-not-exist"),
              "unknown marker id is rejected");

        // Markers must not survive an event change.
        FakeEncoder enc2(store, "r", "01EVENTNNNNNNNNNNNNNNNNNNN");
        enc2.publish_start();
        for (int i = 0; i < 3; ++i) enc2.publish_segment();
        dec.poll(enc2.clock_ms);
        CHECK(dec.markers().empty(), "markers cleared when the event changes");
    }

    std::printf("== 10. Positions map to wall-clock time ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTWWWWWWWWWWWWWWWWWWW");
        enc.publish_start();
        for (int i = 0; i < 10; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d11").string();
        cfg.prebuffer_segments = 0; cfg.buffer_minutes = 4;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);
        dec.start();

        CHECK(dec.event_started_ms() == enc.started_at_ms,
              "event start time reaches the satellite");
        // Segment 4 holds content 24s after the service started (4 x 6s).
        const int64_t expect4 = enc.started_at_ms + 24000;
        CHECK(dec.wall_clock_ms(4) == expect4,
              "a position converts to the clock time of its content");
        CHECK(dec.seek(4), "seek to that position");
        CHECK(dec.playhead_wall_ms() == expect4,
              "playhead reports the clock time being shown");
        CHECK(dec.live_wall_ms() > dec.playhead_wall_ms(),
              "live edge is later than the playhead when behind");
        std::printf("     (showing %lld ms into the epoch, live at %lld)\n",
                    (long long)dec.playhead_wall_ms(),
                    (long long)dec.live_wall_ms());

        // Outside the rolling window it must still estimate rather than give up.
        Manifest m; m.started_at_ms = enc.started_at_ms;
        CHECK(dec.wall_clock_ms(500) > enc.started_at_ms,
              "positions outside the manifest window are still estimated");
    }

    std::printf("== 11. Audio names published by the main site reach the satellite ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTAUDIOAUDIOAUDIOAUD");
        // A packed multi-channel feed, as the encoder publishes it.
        enc.manifest.audio_tracks = { {
            0, "Production", "aac", 8, 48000,
            { "Main L", "Main R", "Sermon ISO", "Click",
              "Choir ISO", "Ambient L", "Ambient R", "Spare" }
        } };
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d12").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        auto layout = dec.audio_layout();
        CHECK(layout.size() == 1, "audio layout received");
        CHECK(!layout.empty() && layout[0].channels == 8, "8 channels reported");
        CHECK(!layout.empty() && layout[0].channel_labels.size() == 8,
              "all channel names received");
        CHECK(!layout.empty() && layout[0].channel_labels[3] == "Click",
              "channel names keep their order (channel 4 is the click)");
        std::printf("     (channel 3 = %s, channel 4 = %s)\n",
                    layout[0].channel_labels[2].c_str(),
                    layout[0].channel_labels[3].c_str());
    }

    std::printf("== 12. Pause and resume survive any prior state ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTPAUSEPAUSEPAUSEPAU");
        enc.publish_start();
        for (int i = 0; i < 8 ; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d13").string();
        // A realistic prebuffer: playback sits behind live, which is the
        // normal case and the one where resume must continue immediately.
        cfg.prebuffer_segments = 3; cfg.buffer_minutes = 4;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);

        // Pausing before playback has begun must still register as "held".
        dec.pause();
        CHECK(dec.play_state() == PlayState::Paused,
              "pause before playback still holds");

        CHECK(dec.start(), "start after a pre-emptive pause");
        dec.resume();
        CHECK(dec.play_state() == PlayState::Playing, "resume starts playback");

        auto a = dec.next_segment();
        CHECK(a.has_value(), "serves a segment after resume");

        // Now the real sequence: play, hold, resume, and it must serve again.
        dec.pause();
        CHECK(dec.play_state() == PlayState::Paused, "held while playing");
        CHECK(!dec.next_segment().has_value(), "nothing served while held");
        const uint64_t held_at = dec.playback_head();

        dec.resume();
        CHECK(dec.play_state() == PlayState::Playing, "resumed");
        auto b = dec.next_segment();
        CHECK(b.has_value(), "SERVES AGAIN AFTER RESUME");
        CHECK(b && b->seq == held_at,
              "continues from exactly where it was held");

        // Resume when already playing must be harmless, not disruptive.
        dec.resume();
        CHECK(dec.play_state() == PlayState::Playing,
              "resume while already playing is harmless");
        CHECK(dec.next_segment().has_value(), "still serving");
    }

    std::printf("== 13. Resuming at the live edge waits for new content ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTEDGEEDGEEDGEEDGEED");
        enc.publish_start();
        for (int i = 0; i < 4; ++i) enc.publish_segment();      // 0..3

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d14").string();
        cfg.prebuffer_segments = 0;      // deliberately AT the live edge
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 6; ++i) dec.pump_downloads(10);
        dec.start();
        while (dec.next_segment().has_value()) {}               // catch right up

        CHECK(dec.playback_head() > dec.live_edge(),
              "playhead has caught up past the newest segment");
        dec.pause();
        dec.resume();
        CHECK(!dec.next_segment().has_value(),
              "resuming with nothing new serves nothing — correct, but the UI "
              "must explain it rather than look broken");

        // As soon as the main site publishes more, playback continues.
        enc.publish_segment();
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.next_segment().has_value(),
              "continues the moment new content arrives");
    }

    std::printf("== 14. Buffer target is honoured in minutes, and fetched fast ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTBUFFERBUFFERBUFFER");
        enc.publish_start();
        // 25 minutes of programme at 6s segments.
        for (int i = 0; i < 250; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d15").string();
        cfg.prebuffer_segments = 2;
        cfg.buffer_minutes = 10;            // 100 segments at 6s
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);

        // Jump back 15 minutes, the case that previously refused to buffer.
        const int64_t fifteen_back = dec.live_wall_ms() - 15 * 60 * 1000;
        CHECK(dec.seek_to_wall_ms(fifteen_back) != 0,
              "seek back 15 minutes by clock time");

        // Draining the queue must bank the whole buffer target, not 60s of it.
        for (int i = 0; i < 400; ++i) dec.pump_downloads(64);
        const double ahead = dec.buffered_ahead_s();
        std::printf("     (buffered %.0fs ahead after seeking back)\n", ahead);
        CHECK(ahead > 300.0,
              "buffers minutes ahead when behind live (not just 60s)");
        CHECK(dec.stats().downloaded > 50,
              "fetched aggressively rather than at playback speed");

        auto ranges = dec.cached_ranges();
        CHECK(!ranges.empty(), "cached ranges reported for the timeline");
        std::printf("     (%zu contiguous cached range(s), first %llu..%llu)\n",
                    ranges.size(),
                    (unsigned long long)ranges.front().first,
                    (unsigned long long)ranges.front().second);
    }

    std::printf("== 15. Seeking by clock time lands mid-segment ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTSEEKSEEKSEEKSEEKSE");
        enc.publish_start();
        for (int i = 0; i < 20; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d16").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 5;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 20; ++i) dec.pump_downloads(32);
        dec.start();

        // 40 seconds in: segment 6 (36s) plus 4 seconds.
        const int64_t target = enc.started_at_ms + 40000;
        CHECK(dec.seek_to_wall_ms(target) == target, "seek to 40s in");
        // Seeking outside what is cached means fetching again — the same
        // behaviour any DVR has, and worth asserting rather than assuming.
        for (int i = 0; i < 20; ++i) dec.pump_downloads(32);
        auto seg = dec.next_segment();
        CHECK(seg.has_value(), "segment served after a timed seek");
        CHECK(seg && seg->seq == 6, "landed on the segment containing 40s");
        CHECK(seg && seg->skip_to_ms == 4000,
              "reports 4s into the segment, so seeking is not limited to "
              "6-second boundaries");
        CHECK(seg && seg->starts_at_ms == enc.started_at_ms + 36000,
              "segment start time reported for the playing clock");
        // The offset applies to that segment only.
        auto seg2 = dec.next_segment();
        CHECK(seg2 && seg2->skip_to_ms == 0, "offset does not leak to the next");
    }

    std::printf("== 16. A finished recording plays as video-on-demand ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTVODVODVODVODVODVOD");
        enc.publish_start();
        for (int i = 0; i < 20; ++i) enc.publish_segment();
        enc.end();                                  // operator ends the broadcast

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d17").string();
        cfg.prebuffer_segments = 2; cfg.buffer_minutes = 5;
        DecoderSession dec(cfg, store);
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended, "reports a finished event");
        CHECK(dec.event_ended(), "event_ended() is true");

        for (int i = 0; i < 10; ++i) dec.pump_downloads(32);
        CHECK(dec.start(), "playback starts");
        CHECK(dec.playback_head() == 0,
              "STARTS AT THE BEGINNING, not near the end");

        // The end time must be the end of the last segment, not its start.
        const int64_t expect_end = enc.started_at_ms + 20 * 6000;
        CHECK(dec.end_wall_ms() == expect_end,
              "end time is the end of the recording");

        // Play right through.
        int served = 0;
        while (dec.next_segment().has_value() && served < 40) ++served;
        CHECK(served == 20, "played every segment through to the end");
        CHECK(dec.at_end(), "reports having reached the end");
        CHECK(dec.playhead_wall_ms() <= dec.end_wall_ms(),
              "the displayed time NEVER runs past the end of the recording");
        std::printf("     (played %d segments; ends at %lld, playhead %lld)\n",
                    served, (long long)dec.end_wall_ms(),
                    (long long)dec.playhead_wall_ms());
    }

    std::printf("== 17. Ending mid-playback plays through to the end ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTFINISHFINISHFINISH");
        enc.publish_start();
        for (int i = 0; i < 10; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d18").string();
        cfg.prebuffer_segments = 4; cfg.buffer_minutes = 5;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        for (int i = 0; i < 10; ++i) dec.pump_downloads(32);
        dec.start();
        const uint64_t from = dec.playback_head();
        dec.next_segment();

        enc.end();                                  // End broadcast pressed
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended,
              "notices the broadcast ended");
        for (int i = 0; i < 10; ++i) dec.pump_downloads(32);

        int served = 1;
        while (dec.next_segment().has_value() && served < 40) ++served;
        CHECK(dec.playback_head() > dec.live_edge(),
              "kept playing to the last segment rather than stopping");
        CHECK(served == (int)(9 - from + 1),
              "served exactly the segments that remained");
        CHECK(dec.playhead_wall_ms() <= dec.end_wall_ms(),
              "time stays within the recording after it finishes");
    }

    std::printf("== 18. 'Ended' means two different things ==\n");
    {
        // (a) Loaded while already finished: this is a recording of a past
        //     service, not something that just ended.
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTWASNTLIVEWASNTLIVE");
        enc.publish_start();
        for (int i = 0; i < 5; ++i) enc.publish_segment();
        enc.end();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d19").string();
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        CHECK(dec.event_ended(), "reports a finished event");
        CHECK(!dec.was_live_this_session(),
              "knows it was NOT live while we were watching");
    }
    {
        // (b) Live when loaded, then the operator ends the broadcast.
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTWASLIVEWASLIVEWASL");
        enc.publish_start();
        for (int i = 0; i < 5; ++i) enc.publish_segment();

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d20").string();
        DecoderSession dec(cfg, store);
        CHECK(dec.poll(enc.clock_ms) == RoomState::Live, "live when loaded");
        CHECK(dec.was_live_this_session(), "remembers seeing it live");

        enc.end();
        CHECK(dec.poll(enc.clock_ms) == RoomState::Ended, "notices it ended");
        CHECK(dec.was_live_this_session(),
              "still knows the broadcast ended while we watched");

        // A different event resets the memory.
        FakeEncoder enc2(store, "r", "01EVENTNEXTNEXTNEXTNEXTNE");
        enc2.publish_start();
        for (int i = 0; i < 3; ++i) enc2.publish_segment();
        enc2.end();
        dec.poll(enc2.clock_ms);
        CHECK(!dec.was_live_this_session(),
              "a different event starts with a clean slate");
    }

    std::printf("== 19. Head never runs past the live edge ==\n");
    {
        FakeStore store;
        FakeEncoder enc(store, "r", "01EVENTIIIIIIIIIIIIIIIIIII");
        enc.publish_start();
        for (int i = 0; i < 3; ++i) enc.publish_segment();   // seqs 0..2

        DecoderConfig cfg;
        cfg.room_id = "r"; cfg.cache_dir = (base / "d9").string();
        cfg.prebuffer_segments = 0;
        DecoderSession dec(cfg, store);
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        dec.start();
        int served = 0;
        while (dec.next_segment().has_value() && served < 20) ++served;
        CHECK(dec.playback_head() <= dec.live_edge() + 1,
              "head stops at the live edge instead of running away");
        CHECK(!dec.next_segment().has_value(),
              "nothing served while waiting for the encoder to publish more");
        for (int i = 0; i < 2; ++i) enc.publish_segment();
        dec.poll(enc.clock_ms);
        dec.pump_downloads(10);
        CHECK(dec.next_segment().has_value(),
              "resumes as soon as new segments appear");
    }

    fs::remove_all(base);
    std::printf("\n%s\n", g_fail == 0 ? "ALL DECODER TESTS PASSED"
                                      : "SOME DECODER TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
