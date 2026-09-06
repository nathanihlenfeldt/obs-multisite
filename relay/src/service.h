#pragma once
//
// service.h — the whole relay, assembled.
//
// Holds the configuration, the one feeder per room, and exactly one
// RelaySession per destination. Sessions are created here and nowhere else,
// which is what makes "two processes pushing to the same destination" not a
// race to be guarded against but a thing that cannot be expressed: starting a
// destination sets a flag on the session that already exists.
//
#include "config_store.h"
#include "relay_session.h"
#include "room_feeder.h"

#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multisite_relay {

struct ServiceStatus {
    bool        storage_configured = false;
    std::string room_id;
    std::string room_state;       // "live", "offline", "ended", "interrupted"
    std::string room_state_text;  // plain language for the banner
    std::string event_id;
    std::string storage_error;
    // What the main site says it is sending, so the UI can offer sound feeds
    // by name and warn before anything is started.
    std::vector<std::string> audio_labels;
    std::string video_summary;    // "1920x1080 H.264"
    bool        can_send = false;
    std::string cannot_send_reason;
    // The number an operator needs when several destinations share one VPS's
    // upload: what is going out in total, right now.
    double      total_out_kbps = 0;
    std::vector<RelayStatus> destinations;
};

class Service {
public:
    ~Service();

    std::string start(const std::string& db_path);
    void stop();

    ConfigStore& config() { return m_cfg; }

    // Brings the running relay into line with the database. Rebuilds the
    // downloader only if the bucket or the room actually changed; otherwise
    // adds, removes and updates sessions in place, so a destination that is
    // on air is not disturbed because a different one was edited.
    void reload();

    ServiceStatus status() const;

    // Test the stored credentials. Empty string means they work.
    std::string check_storage();

    void set_enabled(int64_t id, bool on);

private:
    void supervise();
    void sync_destinations_locked(const std::vector<Destination>& dests,
                                  const RoomSettings& room);

    ConfigStore m_cfg;
    mutable std::mutex m_mtx;
    std::unique_ptr<RoomFeeder> m_feeder;
    std::map<int64_t, std::unique_ptr<RelaySession>> m_sessions;
    // What the current downloader was built from, so a save that changes
    // nothing about the bucket does not rebuild it.
    multisite::S3Config m_feeder_storage;
    std::string         m_feeder_room;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::string m_storage_error;
};

} // namespace multisite_relay
