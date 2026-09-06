#pragma once
//
// config_store.h — everything the relay must remember, in one SQLite file.
//
// One file on a mounted volume is the whole persistent state: which bucket to
// read, which destinations to send to, and what each was told to send. No
// external database, nothing to administer, and a backup is a file copy.
//
// On secrets, plainly: stream keys are stored as they are given, in a file
// created 0600 on a volume the church controls. Encrypting them in the same
// file as the key that decrypts them would be theatre — anyone who can read
// the file can read both. What is done instead is narrower and real: a key is
// never returned by the API, never written to the log, and never included in
// the arguments any status page shows. Stage 3's OAuth refresh token is a
// different matter and is encrypted, because there the secret outlives the
// session and grants far more than one broadcast.
//
#include "destination.h"
#include "s3_transport.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace multisite_relay {

struct RoomSettings {
    std::string room_id = "main-auditorium";
    // The default for destinations that do not set their own. Three minutes:
    // enough to absorb a dropout at the main site without the public stream
    // noticing, and short enough that anyone watching alongside the room is
    // not badly out of step.
    int default_delay_s = 180;
};

class ConfigStore {
public:
    ~ConfigStore();

    // Opens (creating if needed) and brings the schema up to date. Returns a
    // human-readable error, or empty on success.
    std::string open(const std::string& path);

    // ── Storage ──────────────────────────────────────────────────────────────
    multisite::S3Config storage() const;
    void set_storage(const multisite::S3Config& c);
    bool storage_configured() const;

    // ── Room ─────────────────────────────────────────────────────────────────
    RoomSettings room() const;
    void set_room(const RoomSettings& r);

    // ── Destinations ─────────────────────────────────────────────────────────
    std::vector<Destination> destinations() const;
    std::optional<Destination> destination(int64_t id) const;
    // Returns the new id, or 0 with `error` set.
    int64_t add(const Destination& d, std::string& error);
    bool    update(const Destination& d, std::string& error);
    bool    remove(int64_t id);
    void    set_enabled(int64_t id, bool on);

private:
    std::string exec(const std::string& sql);

    sqlite3* m_db = nullptr;
    mutable std::mutex m_mtx;
};

} // namespace multisite_relay
