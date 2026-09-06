// test_relay_state.cpp — the awkward cases, without a destination or a wait.
//
// Everything here is a situation that would otherwise need a real stream key,
// a real outage and two hours of patience to see once. The grace period in
// particular exists because of a measured fact — ffmpeg blocks silently on a
// stalled pipe and will never tell us — so the machine has to notice on its
// own, and that noticing is what these tests pin down.
#include "../src/relay_state.h"

#include <cstdio>
#include <string>

using namespace multisite_relay;
using multisite::RoomState;

static int g_fail = 0;
#define CHECK(c, m) do { if(!(c)){ std::printf("  [FAIL] %s\n", m); ++g_fail; } \
                         else { std::printf("  [ok]   %s\n", m); } } while(0)

static RelayInput live_at(int64_t now, uint64_t latest) {
    RelayInput in;
    in.now_ms = now;
    in.operator_wants_running = true;
    in.room = RoomState::Live;
    in.latest_seq = latest;
    in.first_available_seq = 0;
    in.segment_duration_s = 6.0;
    in.plan_ok = true;
    in.child_alive = false;
    in.next_segment_ready = true;
    in.init_ready = true;
    in.delay_s = 180;
    in.grace_s = 45;
    return in;
}

int main() {
    std::printf("relay state machine\n");

    // ── Taking up position ───────────────────────────────────────────────────
    {
        // 100 segments of 6s exist; a 180s delay is 30 segments back.
        CHECK(seq_behind_live(100, 0, 6.0, 180) == 70,
              "a three-minute delay starts thirty segments behind live");
        CHECK(seq_behind_live(5, 0, 6.0, 180) == 0,
              "a service that just started begins at the beginning");
        CHECK(seq_behind_live(100, 90, 6.0, 180) == 90,
              "never earlier than what storage still holds");
        CHECK(seq_behind_live(100, 0, 6.0, 0) == 100,
              "no delay means the live edge");
    }

    // ── Ordinary running ─────────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(1000, 100);
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Spawn, "it starts ffmpeg when live");
        CHECK(m.head() == 70, "positioned three minutes behind live");
        CHECK(m.state() == RelayState::Streaming, "and reports it is sending");

        in.child_alive = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 70,
              "the first segment goes out immediately");

        d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "the next one is not due yet — this is what holds 1x pacing");

        in.now_ms = 1000 + 6000;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 71,
              "it goes out one segment-duration later");
    }

    // ── A stall shorter than the grace period ────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);                     // spawn
        in.child_alive = true;
        m.step(in);                     // feed 70

        // The main site stops. Segment 71 comes due and is not there — but a
        // segment a moment late is normal near the live edge and must not be
        // reported as a problem.
        in.now_ms = 6000;
        in.next_segment_ready = false;
        auto d = m.step(in);
        CHECK(m.state() == RelayState::Streaming,
              "a segment that is a moment late is not called a stall");

        in.now_ms = 6000 + 3000;
        d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Stalled,
              "three seconds of silence is");

        // Thirty seconds of silence: still riding it out.
        in.now_ms = 36000;
        d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "thirty seconds in, the connection is left alone");
        CHECK(m.state() == RelayState::Stalled, "still stalled");

        // It comes back at forty seconds, inside the grace period.
        in.now_ms = 46000;
        in.next_segment_ready = true;
    in.init_ready = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 71,
              "when content returns it resumes at the very next segment");
        CHECK(m.state() == RelayState::Streaming, "and is sending again");
        CHECK(m.restarts() == 0,
              "a short stall costs no reconnection at all");
    }

    // ── A stall longer than the grace period ─────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);

        in.now_ms = 6000;
        in.next_segment_ready = false;
        m.step(in);                     // content stops at t=6000

        in.now_ms = 6000 + 44000;       // 44s of silence
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "at forty-four seconds it is still holding on");

        in.now_ms = 6000 + 45000;       // exactly the grace period
        d = m.step(in);
        CHECK(d.action == RelayAction::Kill,
              "at forty-five seconds it drops the connection deliberately");
        CHECK(m.state() == RelayState::Reconnecting, "and moves to reconnecting");
        CHECK(!m.last_error().empty(),
              "with a reason the operator can read on screen");
    }

    // ── Running close to the live edge ───────────────────────────────────────
    // The next fragment is routinely a second late. That is not a problem and
    // must not be announced as one, or the screen flickers through a service.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);                     // feed 70

        int announcements = 0;
        for (int cycle = 0; cycle < 5; ++cycle) {
            const int64_t base = 6000 + cycle * 6000;
            in.now_ms = base;           // due, nothing there yet
            in.next_segment_ready = false;
            auto d = m.step(in);
            if (!d.note.empty()) ++announcements;

            in.now_ms = base + 1500;    // it arrives 1.5s late
            in.next_segment_ready = true;
            d = m.step(in);
            if (!d.note.empty()) ++announcements;
            CHECK(d.action == RelayAction::FeedSegment,
                  cycle == 0 ? "a late fragment is still sent" : "and again");
        }
        CHECK(announcements == 0,
              "five late fragments in a row produce no state changes at all");
        CHECK(m.state() == RelayState::Streaming, "and it never left Sending");
    }

    // ── A child that dies on its own ─────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        m.step(in);                     // fed 70

        in.now_ms = 1000;
        in.child_alive = false;         // killed externally
        auto d = m.step(in);
        CHECK(m.state() == RelayState::Reconnecting,
              "an unexpected exit is noticed");
        m.note_child_exited(1000, false);

        // The backoff is served before trying again.
        in.now_ms = 1500;
        d = m.step(in);
        CHECK(d.action == RelayAction::None, "it waits out a short backoff");

        in.now_ms = 2100;
        d = m.step(in);
        CHECK(d.action == RelayAction::Spawn, "then starts again");
        CHECK(m.head() == 71,
              "resuming where it left off, so nothing is skipped");
        CHECK(m.restarts() == 1, "and the restart is counted for the operator");
    }

    // ── A clean end ──────────────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 71);       // only 71 and 72 to send
        in.delay_s = 0;                 // start at the edge for a short test
        m.step(in);
        in.child_alive = true;
        CHECK(m.head() == 71, "positioned at the live edge");
        m.step(in);                     // feed 71

        // The main site publishes one last segment, then ends the broadcast.
        in.room = RoomState::Ended;
        in.latest_seq = 72;
        in.now_ms = 6000;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::FeedSegment && d.seq == 72,
              "the last segment still goes out");

        in.now_ms = 12000;
        d = m.step(in);
        CHECK(d.action == RelayAction::CloseInput,
              "then the pipe is closed so ffmpeg flushes the tail");
        CHECK(m.state() == RelayState::Ending, "and it is finishing off");

        in.child_alive = false;         // ffmpeg finished on its own
        d = m.step(in);
        CHECK(m.state() == RelayState::Stopped, "ending cleanly reaches Stopped");
    }

    // An encoder that died mid-service still gets what it managed to produce.
    {
        RelayMachine m;
        auto in = live_at(0, 70);
        in.delay_s = 0;
        m.step(in);
        in.child_alive = true;
        m.step(in);                     // feed 70
        in.room = RoomState::Interrupted;
        in.now_ms = 6000;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::CloseInput &&
              d.note.find("unexpectedly") != std::string::npos,
              "an interrupted service is sent out and described as cut short");
    }

    // ── Things that must never be sent ───────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.plan_ok = false;
        in.plan_problem = "This service is being recorded as hevc video";
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Blocked,
              "a feed that cannot be sent never spawns anything");
        CHECK(m.last_error() == in.plan_problem,
              "and the reason is what the operator sees");

        // A running relay whose feed turns bad is stopped, not left running.
        RelayMachine m2;
        auto ok = live_at(0, 100);
        m2.step(ok);
        ok.child_alive = true;
        ok.plan_ok = false;
        ok.plan_problem = "the chosen sound feed has gone";
        d = m2.step(ok);
        CHECK(d.action == RelayAction::Kill,
              "a feed that turns bad mid-service is stopped rather than "
              "sending the wrong thing");

        // ...and recovers by itself once it is valid again.
        ok.child_alive = false;
        ok.plan_ok = true;
        d = m2.step(ok);
        CHECK(m2.state() != RelayState::Blocked,
              "and unblocks when the problem goes away");
    }

    // ── The operator's switch ────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        m.step(in);
        in.child_alive = true;
        in.operator_wants_running = false;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::Kill && m.state() == RelayState::Idle,
              "Stop stops it immediately, whatever else is going on");
    }

    // ── The opening data ─────────────────────────────────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.init_ready = false;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Waiting,
              "nothing spawns until the opening data is downloaded");
        // ...but the position must be taken up anyway, because that is what
        // tells the downloader which part of the service to fetch. Waiting
        // for the download before choosing a position is a deadlock: it sits
        // waiting for segments nothing has been asked to bring down.
        CHECK(m.has_position() && m.head() == 70,
              "it still takes up position, so the download knows where to go");

        in.init_ready = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::Spawn,
              "and it starts as soon as that arrives");
    }

    // Segment zero is an ordinary position, not a missing one — a service
    // that has only just started is relayed from its very beginning.
    {
        RelayMachine m;
        auto in = live_at(0, 3);        // 18s old, and a 3-minute delay wanted
        in.init_ready = false;
        m.step(in);
        CHECK(m.has_position() && m.head() == 0,
              "a service younger than the delay starts at segment zero");
        CHECK(m.has_position(), "and that counts as having a position");
    }

    // Nothing is spawned with nothing to send it.
    {
        RelayMachine m;
        auto in = live_at(0, 100);
        in.next_segment_ready = false;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None,
              "no ffmpeg is started before there is content for it");
        in.next_segment_ready = true;
        d = m.step(in);
        CHECK(d.action == RelayAction::Spawn, "only once there is");
    }

    // ── Waiting for a service that has not started ───────────────────────────
    {
        RelayMachine m;
        auto in = live_at(0, 0);
        in.room = RoomState::Offline;
        auto d = m.step(in);
        CHECK(d.action == RelayAction::None && m.state() == RelayState::Waiting,
              "with nothing on air it waits rather than failing");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL RELAY STATE TESTS PASSED"
                                      : "SOME TESTS FAILED");
    return g_fail == 0 ? 0 : 1;
}
