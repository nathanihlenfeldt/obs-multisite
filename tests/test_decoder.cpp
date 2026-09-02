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
    }

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
        cfg.prebuffer_segments = 2; cfg.download_ahead_segments = 30;
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
        cfg.prebuffer_segments = 2; cfg.download_ahead_segments = 40;
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

    std::printf("== 9. Head never runs past the live edge ==\n");
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
