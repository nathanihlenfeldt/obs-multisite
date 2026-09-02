#include "segment_cache.h"
#include "checksum.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace multisite {

static std::string seq_name(uint64_t seq) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08llu", (unsigned long long)seq);
    return b;
}

SegmentCache::SegmentCache(std::string dir, std::string event_id)
    : m_dir(std::move(dir)), m_event_id(std::move(event_id)) {
    ensure_dir();
}

std::string SegmentCache::event_dir() const {
    return (fs::path(m_dir) / m_event_id).string();
}
std::string SegmentCache::seg_path(uint64_t seq) const {
    return (fs::path(event_dir()) / (seq_name(seq) + ".m4s")).string();
}
std::string SegmentCache::init_path() const {
    return (fs::path(event_dir()) / "init.mp4").string();
}
void SegmentCache::ensure_dir() const {
    std::error_code ec;
    fs::create_directories(event_dir(), ec);
}

void SegmentCache::set_event(const std::string& event_id) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (event_id == m_event_id) return;
    // A new event means a new init segment and a fresh sequence space; keeping
    // the old files would risk mixing incompatible codec configs.
    std::error_code ec;
    fs::remove_all(event_dir(), ec);
    m_event_id = event_id;
    ensure_dir();
}

static bool write_atomic(const std::string& path,
                         const std::vector<uint8_t>& bytes) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()),
                (std::streamsize)bytes.size());
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

bool SegmentCache::store_init(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(m_mtx);
    ensure_dir();
    return write_atomic(init_path(), bytes);
}

std::optional<std::vector<uint8_t>> SegmentCache::load_init() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::ifstream f(init_path(), std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

bool SegmentCache::has_init() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    return fs::exists(init_path(), ec);
}

bool SegmentCache::store(uint64_t seq, const std::vector<uint8_t>& bytes,
                         const std::string& expected_checksum) {
    // Verify BEFORE caching: a corrupt segment must never become playable.
    if (!expected_checksum.empty() &&
        !verify_sha256(bytes, expected_checksum)) {
        return false;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    ensure_dir();
    return write_atomic(seg_path(seq), bytes);
}

std::optional<std::vector<uint8_t>> SegmentCache::load(uint64_t seq) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::ifstream f(seg_path(seq), std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}

bool SegmentCache::has(uint64_t seq) const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    return fs::exists(seg_path(seq), ec);
}

std::set<uint64_t> SegmentCache::cached_seqs() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::set<uint64_t> out;
    std::error_code ec;
    if (!fs::exists(event_dir(), ec)) return out;
    for (auto& e : fs::directory_iterator(event_dir(), ec)) {
        if (e.path().extension() != ".m4s") continue;
        try { out.insert(std::stoull(e.path().stem().string())); }
        catch (...) {}
    }
    return out;
}

size_t   SegmentCache::count() const { return cached_seqs().size(); }
uint64_t SegmentCache::lowest_seq() const {
    auto s = cached_seqs();
    return s.empty() ? 0 : *s.begin();
}
uint64_t SegmentCache::highest_seq() const {
    auto s = cached_seqs();
    return s.empty() ? 0 : *s.rbegin();
}

size_t SegmentCache::prune_below(uint64_t keep_from) {
    auto seqs = cached_seqs();
    size_t removed = 0;
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    for (uint64_t s : seqs) {
        if (s >= keep_from) break;
        if (fs::remove(seg_path(s), ec)) ++removed;
    }
    return removed;
}

void SegmentCache::clear() {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::error_code ec;
    fs::remove_all(event_dir(), ec);
    ensure_dir();
}

} // namespace multisite
