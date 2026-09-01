#pragma once
//
// spool_queue.h — durable, crash-safe store-and-forward queue.
//
// The encoder writes each finished segment to disk *before* it is uploaded, so
// nothing is lost if the network drops, OBS crashes, or the machine loses power.
// Segments are drained in strict sequence order; a segment's spool file is only
// removed once the upload is confirmed durable in the bucket.
//
// On-disk layout (all under `dir`):
//   state.json                 event_id, first_seq, last_enqueued, last_confirmed, ended
//   <seq:08d>.seg              raw segment bytes (written tmp+rename → crash-safe)
//   <seq:08d>.meta             JSON sidecar: seq, duration_s, pts_offset_s, checksum, key
//
// This is deliberately dependency-free (no SQLite): plain files with atomic
// write-then-rename give the durability guarantees we need and are trivial to
// reason about and test.
//
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <mutex>

namespace multisite {

struct SpooledSegment {
    uint64_t             seq = 0;
    std::vector<uint8_t> data;
    double               duration_s = 6.0;
    double               pts_offset_s = 0.0;
    std::string          checksum;   // sha256 hex, filled on enqueue
    std::string          key;        // object key it will be uploaded to
};

struct SpoolState {
    std::string event_id;
    uint64_t    first_seq      = 0;
    uint64_t    last_enqueued  = 0;   // highest seq written to spool
    uint64_t    last_confirmed = 0;   // highest seq confirmed durable in bucket
    bool        ended          = false;
    bool        valid          = false; // false if no prior state on disk
};

// Result of opening a spool dir: tells the caller whether a prior, unfinished
// event is present and can be resumed.
struct ResumeInfo {
    bool        resumable = false;
    std::string event_id;
    uint64_t    last_confirmed = 0;
    uint64_t    last_enqueued  = 0;
    size_t      pending_count  = 0;   // segments on disk not yet confirmed
};

class SpoolQueue {
public:
    explicit SpoolQueue(std::string dir);

    // Inspect an existing spool without starting a new event. Used at startup to
    // drive the "resume previous event, or start new?" prompt.
    ResumeInfo inspect() const;

    // Begin a fresh event: clears any old spool and writes new state.
    void begin_event(const std::string& event_id, uint64_t first_seq);

    // Resume the existing on-disk event (keeps pending segments).
    void resume_event();

    // Durably enqueue a segment (atomic write). Computes+stores its checksum.
    // Returns the checksum. Safe to call from the encode thread.
    std::string enqueue(SpooledSegment seg);

    // Lowest-seq pending segment (not yet confirmed), or nullopt if none.
    std::optional<SpooledSegment> peek_next() const;

    // Mark a segment confirmed durable in the bucket → removes its spool files
    // and advances last_confirmed.
    void confirm(uint64_t seq);

    // Number of segments on disk awaiting confirmation.
    size_t pending_count() const;

    // Mark the event ended (clean shutdown) so it is not offered for resume.
    void mark_ended();

    SpoolState state() const;

private:
    std::string m_dir;
    mutable std::mutex m_mtx;
    SpoolState m_state;

    std::string seg_path(uint64_t seq) const;
    std::string meta_path(uint64_t seq) const;
    std::string state_path() const;
    void load_state();
    void save_state();
    std::vector<uint64_t> pending_seqs() const; // sorted ascending
};

} // namespace multisite
