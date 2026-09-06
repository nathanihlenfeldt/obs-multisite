#include "service.h"
#include "log.h"
#include "stream_plan.h"

#include <algorithm>
#include <chrono>

namespace multisite_relay {

using namespace multisite;

Service::~Service() { stop(); }

std::string Service::start(const std::string& db_path) {
    std::string e = m_cfg.open(db_path);
    if (!e.empty()) return e;
    reload();
    m_running = true;
    m_thread = std::thread([this] { supervise(); });
    return {};
}

void Service::stop() {
    if (m_running.exchange(false) && m_thread.joinable()) m_thread.join();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_sessions.clear();          // stops each supervisor and its child
    if (m_feeder) m_feeder->stop();
    m_feeder.reset();
}

// Whether two sets of storage settings describe the same bucket. Rebuilding
// the downloader means dropping the cache and every stream with it, so it must
// happen when the bucket really changed and never merely because something was
// saved.
static bool same_storage(const multisite::S3Config& a,
                         const multisite::S3Config& b) {
    return a.endpoint_host == b.endpoint_host
        && a.r2_account_id == b.r2_account_id
        && a.bucket == b.bucket
        && a.access_key_id == b.access_key_id
        && a.secret_access_key == b.secret_access_key
        && a.region == b.region
        && a.use_https == b.use_https;
}

void Service::reload() {
    const auto storage = m_cfg.storage();
    const auto room = m_cfg.room();
    const auto dests = m_cfg.destinations();

    std::lock_guard<std::mutex> lk(m_mtx);

    // Does the downloader itself have to be rebuilt? Only if the bucket or the
    // room changed. Adding a destination must NOT reach this far: doing so
    // tore down every stream that was already on air, which is precisely what
    // an operator does mid-service when they decide to add Facebook.
    const bool feeder_stale =
        !m_feeder || !same_storage(storage, m_feeder_storage) ||
        room.room_id != m_feeder_room;

    if (!feeder_stale) {
        sync_destinations_locked(dests, room);
        return;
    }

    // Sessions hold a reference to the feeder, so they must go before it does.
    m_sessions.clear();
    if (m_feeder) { m_feeder->stop(); m_feeder.reset(); }

    if (!m_cfg.storage_configured()) {
        m_storage_error = "Storage has not been set up yet.";
        return;
    }
    m_storage_error.clear();

    FeederConfig fc;
    fc.storage = storage;
    fc.room_id = room.room_id;
    fc.cache_dir = "/data/cache";
    if (const char* c = ::getenv("RELAY_CACHE_DIR")) fc.cache_dir = c;

    // The download window has to cover the furthest-behind destination, with
    // room to run ahead of it. A destination sitting three minutes back whose
    // segments were only ever fetched at the live edge would find nothing to
    // send.
    int deepest = room.default_delay_s;
    for (const auto& d : dests)
        deepest = std::max(deepest, d.delay_s > 0 ? d.delay_s : room.default_delay_s);
    fc.buffer_minutes = std::max(10, deepest / 60 + 5);

    m_feeder = std::make_unique<RoomFeeder>(fc);
    m_feeder->start();
    m_feeder_storage = storage;
    m_feeder_room = room.room_id;

    sync_destinations_locked(dests, room);
    rlog_info("watching room \"%s\" with %zu destination(s)",
              room.room_id.c_str(), dests.size());
}

// Bring the running sessions into line with the database, disturbing as little
// as possible. A destination that has not changed keeps its stream, its
// position and its uptime; only what actually differs is acted on.
void Service::sync_destinations_locked(const std::vector<Destination>& dests,
                                       const RoomSettings& room) {
    if (!m_feeder) return;

    std::set<int64_t> seen;
    for (const auto& d : dests) {
        Destination copy = d;
        if (copy.delay_s <= 0) copy.delay_s = room.default_delay_s;
        seen.insert(d.id);

        auto it = m_sessions.find(d.id);
        if (it == m_sessions.end()) {
            auto s = std::make_unique<RelaySession>(copy, *m_feeder);
            s->start_thread();
            m_sessions.emplace(d.id, std::move(s));
            rlog_info("destination \"%s\" added", copy.name.c_str());
            continue;
        }
        // update() decides for itself whether anything here is worth
        // interrupting a live stream for.
        it->second->update(copy);
    }

    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (seen.count(it->first)) { ++it; continue; }
        rlog_info("destination removed");
        it = m_sessions.erase(it);          // its destructor stops the stream
    }
}

void Service::set_enabled(int64_t id, bool on) {
    m_cfg.set_enabled(id, on);
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_sessions.find(id);
    if (it != m_sessions.end()) it->second->set_enabled(on);
}

std::string Service::check_storage() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_feeder) return "Storage has not been set up yet.";
    return m_feeder->check_storage();
}

void Service::supervise() {
    while (m_running) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_feeder) {
                // Keep the download window over the furthest-behind reader, so
                // nothing a destination still needs is pruned underneath it.
                bool any = false;
                uint64_t lowest = 0;
                for (auto& kv : m_sessions) {
                    if (!kv.second->wants_content()) continue;
                    if (!kv.second->has_position()) continue;
                    const uint64_t h = kv.second->head();
                    lowest = any ? std::min(lowest, h) : h;
                    any = true;
                }
                if (any) m_feeder->set_lowest_reader(lowest);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

ServiceStatus Service::status() const {
    ServiceStatus s;
    const auto room = const_cast<ConfigStore&>(m_cfg).room();
    s.room_id = room.room_id;
    s.storage_configured = const_cast<ConfigStore&>(m_cfg).storage_configured();

    std::lock_guard<std::mutex> lk(m_mtx);
    s.storage_error = m_storage_error;
    if (!m_feeder) {
        s.room_state = "offline";
        s.room_state_text = "Storage has not been set up yet";
        return s;
    }

    const auto snap = m_feeder->snapshot();
    s.event_id = snap.event_id;
    switch (snap.room) {
        case RoomState::Live:
            s.room_state = "live";
            s.room_state_text = "A service is on air";
            break;
        case RoomState::Ended:
            s.room_state = "ended";
            s.room_state_text = "The last service has finished";
            break;
        case RoomState::Interrupted:
            s.room_state = "interrupted";
            s.room_state_text = "The main site stopped without ending the service";
            break;
        default:
            s.room_state = "offline";
            s.room_state_text = "Nothing is on air";
            break;
    }
    if (!snap.last_error.empty() && s.storage_error.empty())
        s.storage_error = snap.last_error;

    for (const auto& t : snap.manifest.audio_tracks) s.audio_labels.push_back(t.label);

    if (snap.have_event_info) {
        const auto& v = snap.manifest.video;
        std::string codec = v.codec;
        if (codec == "h264") codec = "H.264";
        else if (codec == "hevc") codec = "HEVC";
        else if (codec == "av1") codec = "AV1";
        if (v.width > 0)
            s.video_summary = std::to_string(v.width) + "x" +
                              std::to_string(v.height) + " " + codec;
        else
            s.video_summary = codec;

        // Answered once for the whole room using a representative
        // destination, so the UI can say "this service cannot be streamed"
        // before an operator sets one up and wonders why it will not start.
        Destination probe;
        probe.name = "probe";
        probe.room_id = room.room_id;
        probe.url = "rtmp://example.invalid/live";
        probe.stream_key = "x";
        const auto plan = plan_stream(snap.manifest, probe, "pipe:0");
        s.can_send = plan.ok;
        if (!plan.ok)
            s.cannot_send_reason = plan.problem +
                                   (plan.remedy.empty() ? "" : " " + plan.remedy);
    }

    for (const auto& kv : m_sessions) {
        auto st = kv.second->status();
        if (st.live) s.total_out_kbps += st.bitrate_kbps;
        s.destinations.push_back(std::move(st));
    }
    return s;
}

} // namespace multisite_relay
