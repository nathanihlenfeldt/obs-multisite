#pragma once
//
// room_feeder.h — one room's receive side, shared by every destination.
//
// There is exactly ONE of these per room, however many places the service is
// being sent to. Each destination having its own poller would multiply the
// bucket reads (and, on a store that bills egress, the bill) by the number of
// destinations, for identical bytes. So the feeder downloads once into one
// verified cache and every relay reads from there.
//
// The receive logic itself is not reimplemented here: DecoderSession already
// does discovery, download-ahead, checksum verification, and the
// live/ended/interrupted classification, and it is the same code a campus
// runs. A second implementation would be a second thing to be subtly wrong.
// This adds only what a relay needs and a player does not: keeping the
// download window parked over the delayed position the destinations are
// actually reading from.
//
#include "decoder_session.h"
#include "model.h"
#include "s3_transport.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace multisite_relay {

struct FeederConfig {
    multisite::S3Config storage;
    std::string room_id = "main-auditorium";
    std::string cache_dir = "/data/cache";
    int poll_interval_ms = 3000;
    // Enough to cover the largest delay any destination is using, plus room to
    // run ahead. Set from the configured delays at startup.
    int buffer_minutes = 15;
    int stale_after_ms = 600000;      // the same 10 minutes a campus uses
};

// A consistent view of the room, taken under one lock so a caller cannot see
// the live edge from one moment and the audio layout from another.
struct RoomSnapshot {
    multisite::RoomState room = multisite::RoomState::Unknown;
    std::string event_id;
    // Shaped as a Manifest because that is what plan_stream() reads. Video and
    // segment duration come from the event descriptor (static, written once);
    // the audio layout and the sequence numbers come from the live manifest.
    multisite::Manifest manifest;
    bool     have_event_info = false;
    uint64_t latest_seq = 0;
    uint64_t first_available_seq = 0;
    double   segment_duration_s = 6.0;
    std::string last_error;
    uint64_t downloaded = 0;
    uint64_t checksum_failures = 0;
};

class RoomFeeder {
public:
    explicit RoomFeeder(FeederConfig cfg);
    ~RoomFeeder();

    void start();
    void stop();

    RoomSnapshot snapshot() const;

    // Is this segment on disk and verified, ready to hand to ffmpeg?
    bool has_segment(uint64_t seq) const;
    std::optional<std::vector<uint8_t>> load_segment(uint64_t seq) const;
    std::optional<std::vector<uint8_t>> load_init() const;

    // Where the destinations are actually reading from. The download window is
    // parked here rather than at the live edge, or a relay sitting three
    // minutes back would find its segments already pruned.
    void set_lowest_reader(uint64_t seq);

    // A one-off credential and connectivity check, so a mistyped key is
    // reported when it is entered rather than when a service starts.
    std::string check_storage();

private:
    void run();

    FeederConfig m_cfg;
    std::unique_ptr<multisite::S3Transport> m_tx;
    std::unique_ptr<multisite::DecoderSession> m_session;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_lowest_reader{0};
    std::atomic<bool> m_reader_set{false};

    // event.json is static for the life of an event, so it is fetched once
    // rather than on every poll.
    mutable std::mutex m_mtx;
    std::string  m_info_event_id;
    multisite::EventInfo m_info;
    bool m_have_info = false;
};

} // namespace multisite_relay
