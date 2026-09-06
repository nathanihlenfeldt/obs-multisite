#pragma once
//
// relay_state.h — one destination's lifecycle, as a pure state machine.
//
// No threads, no subprocesses, no clock of its own: every input arrives as an
// argument and every decision comes back as a return value. That is what makes
// the awkward cases — a 45-second stall, a child that died at the worst
// moment, an encoder that stopped without saying so — testable without a
// destination, a bucket, or a two-hour wait. The supervisor in
// relay_session.cpp does the actual spawning and feeding; it holds no policy.
//
// The two findings this is built around, both measured in the Stage 0 rig
// rather than assumed:
//
//   1. ffmpeg does not report starvation. Given a pipe that stops producing,
//      it blocks silently, holds the socket open and sends nothing — no error,
//      no exit, indefinitely. So a stall can only be noticed HERE, by watching
//      whether content is still arriving. Waiting for the child to complain is
//      waiting for ever.
//
//   2. A stall costs nothing if it is short. Fragment timestamps are absolute,
//      so content resumes exactly where it left off with no gap and no
//      discontinuity. A destination that tolerates the pause never knows. That
//      is why a stall is ridden out for a grace period rather than answered
//      immediately with a teardown: tearing down early splits the recording
//      for a hiccup that would have been invisible.
//
#include "decoder_session.h"   // RoomState: the same live/ended/interrupted
                                 // classification the decoder uses, so the
                                 // relay and a campus never disagree about
                                 // whether a service is still running.
#include "model.h"

#include <cstdint>
#include <string>

namespace multisite_relay {

enum class RelayState {
    Idle,          // the operator has not started this destination
    Waiting,       // started, but there is nothing to send yet
    Streaming,     // content is flowing
    Stalled,       // nothing new has arrived; riding out the grace period
    Reconnecting,  // torn down, waiting to try again
    Ending,        // the broadcast finished; sending what is left
    Stopped,       // finished, everything sent
    Blocked,       // cannot be sent at all until someone changes something
};

const char* to_string(RelayState s);

// Plain language for the operator's status line. Never mentions segments,
// pipes, or ffmpeg.
std::string describe(RelayState s);

enum class RelayAction {
    None,
    Spawn,        // start ffmpeg (the caller builds it from the StreamPlan)
    FeedSegment,  // write `seq` into the pipe now
    CloseInput,   // no more content: close the pipe and let ffmpeg finish
    Kill,         // stop ffmpeg now, without waiting
};

// Everything the machine is allowed to know, gathered by the caller.
struct RelayInput {
    int64_t now_ms = 0;

    bool operator_wants_running = false;   // the destination is enabled

    multisite::RoomState room = multisite::RoomState::Unknown;
    uint64_t latest_seq = 0;               // the live edge
    uint64_t first_available_seq = 0;      // the retention floor
    double   segment_duration_s = 6.0;

    // Whether plan_stream() currently succeeds. A feed that turns out to be
    // HEVC, or whose chosen track has vanished, must stop the relay rather
    // than send something wrong.
    bool        plan_ok = false;
    std::string plan_problem;

    bool child_alive = false;

    // Whether the segment the machine last asked for is now in the cache.
    // Answered by the caller because only it can see the disk.
    bool next_segment_ready = false;

    // Whether the event's init segment is cached. Nothing can be spawned
    // without it: it carries the codec configuration, and ffmpeg fed
    // fragments alone would exit rather than wait for one.
    bool init_ready = false;

    // How far behind the live edge this destination should sit.
    int delay_s = 180;

    // How long a silence to ride out before giving up on the connection.
    // Measured against the destination's own tolerance: YouTube ends a
    // broadcast after roughly 60 seconds without data, so this sits under it.
    int grace_s = 45;
};

struct RelayDecision {
    RelayAction action = RelayAction::None;
    uint64_t    seq = 0;          // for FeedSegment
    // Set when the state changed, so the caller logs transitions rather than
    // every tick. Empty otherwise.
    std::string note;
};

class RelayMachine {
public:
    // Call as often as convenient (ten times a second is plenty); it does
    // nothing until something is due. One action per call, so a caller that
    // wants to drain several does so by calling again.
    RelayDecision step(const RelayInput& in);

    RelayState state() const { return m_state; }
    uint64_t   head() const { return m_head; }
    // Segment 0 is a perfectly ordinary position, so "have we taken one up"
    // cannot be inferred from the number being zero.
    bool       has_position() const { return m_head_set; }

    // The last thing that went wrong, for the status panel. Errors belong on
    // screen, not only in a log.
    const std::string& last_error() const { return m_last_error; }

    // How far behind live this destination actually is right now, in seconds,
    // or -1 when it is not sending. This is what the operator is shown; it is
    // the target delay plus whatever a restart cost.
    double behind_live_s(const RelayInput& in) const;

    // Reported so the operator can see a destination that keeps dropping,
    // rather than only its current state.
    int restarts() const { return m_restarts; }

    void note_child_exited(int64_t now_ms, bool expected);

private:
    void enter(RelayState s, int64_t now_ms, const std::string& why);

    RelayState m_state = RelayState::Idle;
    uint64_t   m_head = 0;
    bool       m_head_set = false;

    // 1x pacing is anchored at the moment of the last spawn: segment N is due
    // one segment-duration after segment N-1, never sooner. Releasing
    // everything that is "overdue" after a restart would flood the destination
    // with a burst it did not ask for.
    int64_t  m_anchor_ms = 0;
    uint64_t m_anchor_seq = 0;

    int64_t m_stalled_since_ms = 0;
    // When content first went missing, as distinct from when we started
    // calling it a stall.
    int64_t m_overdue_since_ms = 0;
    int64_t m_retry_at_ms = 0;
    int     m_backoff_ms = 0;
    int     m_restarts = 0;
    int64_t m_streaming_since_ms = 0;

    std::string m_last_error;
};

// Which segment to start from, so the destination sits `delay_s` behind the
// live edge. Clamped to what storage still holds: asking for three minutes ago
// on a service that started ninety seconds ago starts at the beginning.
uint64_t start_seq_for_delay(const multisite::Manifest& m, int delay_s);

// The same arithmetic without a manifest, for the machine's own use.
uint64_t seq_behind_live(uint64_t latest_seq, uint64_t first_available_seq,
                         double seg_s, int delay_s);

} // namespace multisite_relay
