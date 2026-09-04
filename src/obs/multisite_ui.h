#pragma once
//
// multisite_ui.h — the interfaces the frontend controls act through.
//
// The encoder output and decoder sources register themselves here; hotkeys and
// Tools-menu items then work on whatever is currently live, without holding
// raw pointers to objects that come and go.
//
#include <string>
#include <utility>
#include <vector>

namespace multisite_obs {

// Implemented by the encoder output while broadcasting.
struct EncoderStats {
    std::string event_id;
    unsigned long long confirmed = 0;
    unsigned long long pending = 0;
    unsigned long long retries = 0;
    unsigned long long bytes = 0;
    int         link_health = 0;      // 0 healthy, 1 degraded, 2 offline
    std::string last_error;
};

struct EncoderControls {
    virtual ~EncoderControls() = default;
    virtual void drop_marker(const std::string& label) = 0;
    virtual std::string marker_labels() const = 0;   // comma-separated
    virtual void log_status() = 0;
    // Live figures for the dock: queue depth, retries, link health.
    virtual EncoderStats stats() const = 0;
};

// Implemented by each decoder source.
struct DecoderSnapshot;   // defined below

struct DecoderControls {
    virtual ~DecoderControls() = default;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void toggle_pause() = 0;
    virtual void jump_to_live() = 0;
    virtual void log_status() = 0;
    virtual void snapshot(DecoderSnapshot& out) const = 0;
    virtual void jump_to_marker(const std::string& id) = 0;
    virtual void seek(unsigned long long seq) = 0;
    // Re-read settings (including the machine-wide storage config) and
    // restart. Needed when credentials are entered in the dock after a source
    // already exists.
    virtual void reconfigure() = 0;
};

void register_encoder_controls(EncoderControls* e);
void unregister_encoder_controls(EncoderControls* e);
void register_decoder_controls(DecoderControls* d);
void unregister_decoder_controls(DecoderControls* d);

void register_ui();
void unregister_ui();

// Reaches whichever encoder is currently broadcasting, if any.
bool encoder_stats(EncoderStats& out);
void forward_marker_to_encoder(const std::string& label);

// Snapshot of the decoder sources, for the decoder dock.
struct DecoderSnapshot {
    std::string room_id;
    int         room_state = 0;       // matches RoomState
    unsigned long long head = 0, live_edge = 0, first_available = 0;
    double      behind_live_s = 0.0, buffered_ahead_s = 0.0;
    bool        paused = false;
    size_t      cached = 0;
    std::string current_marker;
    std::string last_error;
    int         audio_channels = 0;
    // Wall-clock times, so the UI never has to mention sequence numbers.
    long long   playhead_ms = 0, live_ms = 0, earliest_ms = 0, started_ms = 0;
    // label, id, and the clock time the marker refers to
    struct MarkerEntry { std::string label; std::string id; long long at_ms; };
    std::vector<MarkerEntry> markers;
};
bool decoder_snapshot(DecoderSnapshot& out);
void decoder_pause_all();
void decoder_resume_all();
void decoder_jump_live_all();
void decoder_reconfigure_all();
void decoder_jump_to_marker(const std::string& id);
void decoder_seek(unsigned long long seq);

} // namespace multisite_obs
