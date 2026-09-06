#pragma once
//
// log.h — the relay's log.
//
// Goes to stderr, which is what `docker logs` shows, so a church's integrator
// with nothing but a terminal can see why a stream is not going out. The last
// few hundred lines are also kept in memory and served to the browser: errors
// belong on screen, not only in a log, and the person who needs them is
// usually holding a phone rather than an SSH session.
//
// Deliberately a copy of the appliance's logger rather than a shared one: the
// relay is a separate sub-project and must not make src/core depend on it, or
// on anything it introduces.
//
#include <string>
#include <vector>

namespace multisite_relay {

enum class LogLevel { Debug, Info, Warn, Error };

void log_line(LogLevel level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define rlog_debug(...) ::multisite_relay::log_line(::multisite_relay::LogLevel::Debug, __VA_ARGS__)
#define rlog_info(...)  ::multisite_relay::log_line(::multisite_relay::LogLevel::Info,  __VA_ARGS__)
#define rlog_warn(...)  ::multisite_relay::log_line(::multisite_relay::LogLevel::Warn,  __VA_ARGS__)
#define rlog_error(...) ::multisite_relay::log_line(::multisite_relay::LogLevel::Error, __VA_ARGS__)

void set_debug_logging(bool on);

struct LogEntry {
    long long   at_ms = 0;
    LogLevel    level = LogLevel::Info;
    std::string text;
};

// Newest last. Copied out under the lock.
std::vector<LogEntry> recent_log(size_t max_lines = 200);

const char* to_string(LogLevel l);

} // namespace multisite_relay
