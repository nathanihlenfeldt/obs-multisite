#include "config_store.h"
#include "log.h"

#include <cstdint>
#include <sqlite3.h>
#include <sys/stat.h>

namespace multisite_relay {

namespace {

std::string text_col(sqlite3_stmt* st, int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? reinterpret_cast<const char*>(t) : "";
}

void bind_text(sqlite3_stmt* st, int i, const std::string& s) {
    sqlite3_bind_text(st, i, s.c_str(), -1, SQLITE_TRANSIENT);
}

} // namespace

ConfigStore::~ConfigStore() {
    if (m_db) sqlite3_close(m_db);
}

std::string ConfigStore::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "unknown error";
        sqlite3_free(err);
        return e;
    }
    return {};
}

std::string ConfigStore::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (sqlite3_open(path.c_str(), &m_db) != SQLITE_OK)
        return "could not open " + path + ": " +
               (m_db ? sqlite3_errmsg(m_db) : "unknown error");

    // The file holds stream keys, so it is not world-readable even on a
    // volume somebody else can list.
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);

    // WAL, so a read for the status page never waits behind a write.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA busy_timeout=3000;");

    std::string e = exec(
        "CREATE TABLE IF NOT EXISTS settings ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS destinations ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL,"
        "  room_id TEXT NOT NULL,"
        "  url TEXT NOT NULL,"
        "  stream_key TEXT NOT NULL DEFAULT '',"
        "  audio_label TEXT NOT NULL DEFAULT '',"
        "  allow_transcode INTEGER NOT NULL DEFAULT 0,"
        "  enabled INTEGER NOT NULL DEFAULT 0,"
        "  delay_s INTEGER NOT NULL DEFAULT 0"
        ");");
    if (!e.empty()) return "could not prepare the database: " + e;
    return {};
}

// ── settings helpers ─────────────────────────────────────────────────────────

namespace {

std::string get_setting(sqlite3* db, const std::string& key,
                        const std::string& fallback = "") {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key=?;", -1,
                           &st, nullptr) != SQLITE_OK)
        return fallback;
    bind_text(st, 1, key);
    std::string out = fallback;
    if (sqlite3_step(st) == SQLITE_ROW) out = text_col(st, 0);
    sqlite3_finalize(st);
    return out;
}

void put_setting(sqlite3* db, const std::string& key, const std::string& value) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO settings(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &st, nullptr) != SQLITE_OK)
        return;
    bind_text(st, 1, key);
    bind_text(st, 2, value);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

} // namespace

std::string ConfigStore::get_secret(const std::string& key) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return get_setting(m_db, key);
}

void ConfigStore::set_secret(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, key, value);
}

multisite::S3Config ConfigStore::storage() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    multisite::S3Config c;
    c.endpoint_host      = get_setting(m_db, "endpoint_host");
    c.r2_account_id      = get_setting(m_db, "r2_account_id");
    c.bucket             = get_setting(m_db, "bucket");
    c.access_key_id      = get_setting(m_db, "access_key_id");
    c.secret_access_key  = get_setting(m_db, "secret_access_key");
    c.region             = get_setting(m_db, "region", "auto");
    // Plain HTTP is refused by every hosted store, but a self-hosted MinIO on
    // the church's own LAN often has no certificate. Off by default, so it is
    // a deliberate choice rather than an accident.
    c.use_https          = get_setting(m_db, "use_https", "1") != "0";
    return c;
}

void ConfigStore::set_storage(const multisite::S3Config& c) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "endpoint_host", c.endpoint_host);
    put_setting(m_db, "r2_account_id", c.r2_account_id);
    put_setting(m_db, "bucket", c.bucket);
    put_setting(m_db, "access_key_id", c.access_key_id);
    put_setting(m_db, "secret_access_key", c.secret_access_key);
    put_setting(m_db, "region", c.region.empty() ? "auto" : c.region);
    put_setting(m_db, "use_https", c.use_https ? "1" : "0");
}

bool ConfigStore::storage_configured() const {
    const auto c = storage();
    return !c.bucket.empty() && !c.access_key_id.empty() &&
           (!c.endpoint_host.empty() || !c.r2_account_id.empty());
}

RoomSettings ConfigStore::room() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    RoomSettings r;
    r.room_id = get_setting(m_db, "room_id", "main-auditorium");
    const std::string d = get_setting(m_db, "default_delay_s", "180");
    try { r.default_delay_s = std::stoi(d); } catch (...) {}
    return r;
}

void ConfigStore::set_room(const RoomSettings& r) {
    std::lock_guard<std::mutex> lk(m_mtx);
    put_setting(m_db, "room_id", r.room_id);
    put_setting(m_db, "default_delay_s", std::to_string(r.default_delay_s));
}

// ── destinations ─────────────────────────────────────────────────────────────

namespace {
Destination read_row(sqlite3_stmt* st) {
    Destination d;
    d.id              = sqlite3_column_int64(st, 0);
    d.name            = text_col(st, 1);
    d.room_id         = text_col(st, 2);
    d.url             = text_col(st, 3);
    d.stream_key      = text_col(st, 4);
    d.audio.label     = text_col(st, 5);
    d.allow_transcode = sqlite3_column_int(st, 6) != 0;
    d.enabled         = sqlite3_column_int(st, 7) != 0;
    d.delay_s         = sqlite3_column_int(st, 8);
    return d;
}
const char* kSelect =
    "SELECT id,name,room_id,url,stream_key,audio_label,allow_transcode,"
    "enabled,delay_s FROM destinations";
} // namespace

std::vector<Destination> ConfigStore::destinations() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<Destination> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, (std::string(kSelect) + " ORDER BY id;").c_str(),
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(read_row(st));
    sqlite3_finalize(st);
    return out;
}

std::optional<Destination> ConfigStore::destination(int64_t id) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, (std::string(kSelect) + " WHERE id=?;").c_str(),
                           -1, &st, nullptr) != SQLITE_OK)
        return std::nullopt;
    sqlite3_bind_int64(st, 1, id);
    std::optional<Destination> out;
    if (sqlite3_step(st) == SQLITE_ROW) out = read_row(st);
    sqlite3_finalize(st);
    return out;
}

int64_t ConfigStore::add(const Destination& d, std::string& error) {
    error = validate(d);
    if (!error.empty()) return 0;

    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT INTO destinations(name,room_id,url,stream_key,audio_label,"
            "allow_transcode,enabled,delay_s) VALUES(?,?,?,?,?,?,?,?);",
            -1, &st, nullptr) != SQLITE_OK) {
        error = "could not save this destination";
        return 0;
    }
    bind_text(st, 1, d.name);
    bind_text(st, 2, d.room_id);
    bind_text(st, 3, d.url);
    bind_text(st, 4, d.stream_key);
    bind_text(st, 5, d.audio.label);
    sqlite3_bind_int(st, 6, d.allow_transcode ? 1 : 0);
    sqlite3_bind_int(st, 7, d.enabled ? 1 : 0);
    sqlite3_bind_int(st, 8, d.delay_s);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { error = "could not save this destination"; return 0; }
    return sqlite3_last_insert_rowid(m_db);
}

bool ConfigStore::update(const Destination& d, std::string& error) {
    error = validate(d);
    if (!error.empty()) return false;

    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "UPDATE destinations SET name=?,room_id=?,url=?,stream_key=?,"
            "audio_label=?,allow_transcode=?,enabled=?,delay_s=? WHERE id=?;",
            -1, &st, nullptr) != SQLITE_OK) {
        error = "could not save this destination";
        return false;
    }
    bind_text(st, 1, d.name);
    bind_text(st, 2, d.room_id);
    bind_text(st, 3, d.url);
    bind_text(st, 4, d.stream_key);
    bind_text(st, 5, d.audio.label);
    sqlite3_bind_int(st, 6, d.allow_transcode ? 1 : 0);
    sqlite3_bind_int(st, 7, d.enabled ? 1 : 0);
    sqlite3_bind_int(st, 8, d.delay_s);
    sqlite3_bind_int64(st, 9, d.id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) error = "could not save this destination";
    return ok;
}

bool ConfigStore::remove(int64_t id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM destinations WHERE id=?;", -1,
                           &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

void ConfigStore::set_enabled(int64_t id, bool on) {
    std::lock_guard<std::mutex> lk(m_mtx);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(m_db, "UPDATE destinations SET enabled=? WHERE id=?;",
                           -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_int(st, 1, on ? 1 : 0);
    sqlite3_bind_int64(st, 2, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

} // namespace multisite_relay
