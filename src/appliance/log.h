#pragma once
//
// log.h — the appliance's log.
//
// Goes to stderr, which systemd captures into the journal, so `journalctl -u
// multisite-player` is the whole diagnostic story on a box with no screen
// attached. The last few hundred lines are also kept in memory and served to
// the web UI: an operator with a phone and no SSH should still be able to see
// why nothing is playing.
//
#include <string>
#include <vector>

namespace multisite_player {

enum class LogLevel { Debug, Info, Warn, Error };

void log_line(LogLevel level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define plog_debug(...) ::multisite_player::log_line(::multisite_player::LogLevel::Debug, __VA_ARGS__)
#define plog_info(...)  ::multisite_player::log_line(::multisite_player::LogLevel::Info,  __VA_ARGS__)
#define plog_warn(...)  ::multisite_player::log_line(::multisite_player::LogLevel::Warn,  __VA_ARGS__)
#define plog_error(...) ::multisite_player::log_line(::multisite_player::LogLevel::Error, __VA_ARGS__)

void set_debug_logging(bool on);

struct LogEntry {
    long long   at_ms = 0;
    LogLevel    level = LogLevel::Info;
    std::string text;
};

// Newest last. Copied out under the lock.
std::vector<LogEntry> recent_log(size_t max_lines = 200);

const char* to_string(LogLevel l);

} // namespace multisite_player
