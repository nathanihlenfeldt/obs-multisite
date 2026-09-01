#include "spool_queue.h"
#include "checksum.h"
#include "../vendor/nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

SpoolQueue::SpoolQueue(std::string dir) : m_dir(std::move(dir)) {
    fs::create_directories(m_dir);
    load_state();
}

std::string SpoolQueue::seg_path(uint64_t seq) const {
    return (fs::path(m_dir) / (seq_name(seq) + ".seg")).string();
}
std::string SpoolQueue::meta_path(uint64_t seq) const {
    return (fs::path(m_dir) / (seq_name(seq) + ".meta")).string();
}
std::string SpoolQueue::state_path() const {
    return (fs::path(m_dir) / "state.json").string();
}

// Atomic write: write to <path>.tmp then rename over <path>.
static void atomic_write(const std::string& path, const void* data, size_t n) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("spool: cannot open " + tmp);
        f.write(reinterpret_cast<const char*>(data), (std::streamsize)n);
        f.flush();
    }
    fs::rename(tmp, path); // atomic on POSIX and Windows same-volume
}

void SpoolQueue::load_state() {
    m_state = SpoolState{};
    std::ifstream f(state_path());
    if (!f) return;
    try {
        json j; f >> j;
        m_state.event_id       = j.value("event_id", "");
        m_state.first_seq      = j.value("first_seq", (uint64_t)0);
        m_state.last_enqueued  = j.value("last_enqueued", (uint64_t)0);
        m_state.last_confirmed = j.value("last_confirmed", (uint64_t)0);
        m_state.ended          = j.value("ended", false);
        m_state.valid          = true;
    } catch (...) {
        m_state = SpoolState{}; // corrupt state → treat as none
    }
}

void SpoolQueue::save_state() {
    json j;
    j["event_id"]       = m_state.event_id;
    j["first_seq"]      = m_state.first_seq;
    j["last_enqueued"]  = m_state.last_enqueued;
    j["last_confirmed"] = m_state.last_confirmed;
    j["ended"]          = m_state.ended;
    std::string s = j.dump();
    atomic_write(state_path(), s.data(), s.size());
}

std::vector<uint64_t> SpoolQueue::pending_seqs() const {
    std::vector<uint64_t> out;
    if (!fs::exists(m_dir)) return out;
    for (auto& e : fs::directory_iterator(m_dir)) {
        auto p = e.path();
        if (p.extension() == ".seg") {
            try { out.push_back(std::stoull(p.stem().string())); }
            catch (...) {}
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

ResumeInfo SpoolQueue::inspect() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    ResumeInfo r;
    if (!m_state.valid || m_state.ended) return r;
    auto pend = pending_seqs();
    // Resumable if there is a live (un-ended) event, whether or not segments
    // are still pending (operator may want to continue the sequence).
    r.resumable      = true;
    r.event_id       = m_state.event_id;
    r.last_confirmed = m_state.last_confirmed;
    r.last_enqueued  = m_state.last_enqueued;
    r.pending_count  = pend.size();
    return r;
}

void SpoolQueue::begin_event(const std::string& event_id, uint64_t first_seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // clear old spool
    for (auto& e : fs::directory_iterator(m_dir)) {
        auto ext = e.path().extension();
        if (ext == ".seg" || ext == ".meta") fs::remove(e.path());
    }
    m_state = SpoolState{};
    m_state.event_id      = event_id;
    m_state.first_seq     = first_seq;
    m_state.last_enqueued = first_seq > 0 ? first_seq - 1 : 0;
    m_state.last_confirmed= first_seq > 0 ? first_seq - 1 : 0;
    m_state.ended         = false;
    m_state.valid         = true;
    save_state();
}

void SpoolQueue::resume_event() {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_state.valid) throw std::runtime_error("spool: nothing to resume");
    m_state.ended = false;
    save_state();
}

std::string SpoolQueue::enqueue(SpooledSegment seg) {
    std::lock_guard<std::mutex> lk(m_mtx);
    seg.checksum = sha256_hex(seg.data);

    // 1) segment bytes (atomic)
    atomic_write(seg_path(seg.seq), seg.data.data(), seg.data.size());
    // 2) sidecar meta (atomic) — written AFTER the bytes so a crash between the
    //    two leaves an orphan .seg with no .meta, which drain skips safely.
    json m;
    m["seq"]          = seg.seq;
    m["duration_s"]   = seg.duration_s;
    m["pts_offset_s"] = seg.pts_offset_s;
    m["checksum"]     = seg.checksum;
    m["key"]          = seg.key;
    std::string ms = m.dump();
    atomic_write(meta_path(seg.seq), ms.data(), ms.size());

    if (seg.seq > m_state.last_enqueued) m_state.last_enqueued = seg.seq;
    save_state();
    return seg.checksum;
}

std::optional<SpooledSegment> SpoolQueue::peek_next() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    for (uint64_t seq : pending_seqs()) {
        std::string mp = meta_path(seq), sp = seg_path(seq);
        if (!fs::exists(mp)) continue; // orphaned .seg (crash between writes)
        std::ifstream mf(mp);
        json m; try { mf >> m; } catch (...) { continue; }

        std::ifstream sf(sp, std::ios::binary);
        if (!sf) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(sf)), {});

        SpooledSegment s;
        s.seq          = seq;
        s.data         = std::move(data);
        s.duration_s   = m.value("duration_s", 6.0);
        s.pts_offset_s = m.value("pts_offset_s", 0.0);
        s.checksum     = m.value("checksum", "");
        s.key          = m.value("key", "");
        return s;
    }
    return std::nullopt;
}

void SpoolQueue::confirm(uint64_t seq) {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    fs::remove(seg_path(seq), ec);
    fs::remove(meta_path(seq), ec);
    if (seq > m_state.last_confirmed) m_state.last_confirmed = seq;
    save_state();
}

size_t SpoolQueue::pending_count() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return pending_seqs().size();
}

void SpoolQueue::mark_ended() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_state.ended = true;
    save_state();
}

SpoolState SpoolQueue::state() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_state;
}

} // namespace multisite
