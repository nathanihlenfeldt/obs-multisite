#pragma once
//
// relay_session.h — one destination, supervised.
//
// Owns the only ffmpeg child that will ever push to this destination, and the
// only thread that drives it. That ownership IS the idempotency guarantee the
// brief asks for: there is no code path that can produce a second process for
// the same destination, because starting one means setting a flag on the
// session that already exists rather than creating anything.
//
// All policy lives in RelayMachine; all process handling lives in
// FfmpegProcess. This is the loop that connects them to the bucket, plus the
// bookkeeping the operator's screen needs.
//
#include "destination.h"
#include "ffmpeg_process.h"
#include "relay_state.h"
#include "room_feeder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_relay {

// What the UI shows. Deliberately in plain language and finished units: the
// browser renders these, it does not compute them.
struct RelayStatus {
    int64_t     id = 0;
    std::string name;
    std::string state;          // machine-readable, for styling
    std::string state_text;     // "Sending", "Reconnecting"
    std::string detail;         // "sending 1920x1080 H.264 with 'Main Mix'"
    std::string error;          // shown on screen, not just logged
    bool        enabled = false;
    bool        live = false;   // actually pushing right now
    int64_t     uptime_s = 0;
    double      behind_live_s = -1;
    double      bitrate_kbps = 0;
    int         restarts = 0;
    int64_t     sent_bytes = 0;
    std::string audio_label;
};

class RelaySession {
public:
    RelaySession(Destination dest, RoomFeeder& feeder);
    ~RelaySession();

    void start_thread();
    void stop_thread();

    // The operator's switch. Takes effect on the next pass of the loop; it
    // does not spawn or kill anything itself, so there is one place that does.
    void set_enabled(bool on);
    bool enabled() const { return m_enabled; }

    // Replaces the configuration (name, audio choice, delay). A change that
    // affects what is being sent restarts the stream, because the destination
    // cannot be switched mid-flight.
    void update(const Destination& d);

    Destination destination() const;
    RelayStatus status() const;

    // Where this destination has read up to, so the feeder knows what it must
    // keep and what it may prune.
    uint64_t head() const { return m_machine.head(); }
    bool     has_position() const { return m_machine.has_position(); }
    bool     wants_content() const { return m_enabled; }

private:
    void run();
    void pump_writes();
    // Separate on purpose: the init segment goes in at spawn, and the media
    // fragments come only from the machine's FeedSegment decisions. Queueing
    // a fragment at spawn as well sent the first one twice.
    bool queue_init();
    bool queue_segment(uint64_t seq);

    Destination  m_dest;
    RoomFeeder&  m_feeder;
    RelayMachine m_machine;
    std::unique_ptr<FfmpegProcess> m_child;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_enabled{false};

    // Content waiting to go into the pipe. A fragment is several megabytes and
    // the pipe takes it in 64 KB bites, so this is normal, not a backlog.
    std::vector<uint8_t> m_pending;
    size_t m_pending_offset = 0;

    mutable std::mutex m_mtx;
    std::string m_detail;
    std::string m_error;
    std::string m_audio_label;
    int64_t m_started_ms = 0;
    int64_t m_sent_bytes = 0;
    double  m_bitrate_kbps = 0;
};

} // namespace multisite_relay
