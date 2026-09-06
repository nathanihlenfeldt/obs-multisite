#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace multisite_relay {

namespace {
std::mutex g_mtx;
std::deque<LogEntry> g_lines;
bool g_debug = false;
constexpr size_t kKeep = 500;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
} // namespace

const char* to_string(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}

void set_debug_logging(bool on) { g_debug = on; }

void log_line(LogLevel level, const char* fmt, ...) {
    if (level == LogLevel::Debug && !g_debug) return;

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::fprintf(stderr, "[%s] %s\n", to_string(level), buf);
    std::fflush(stderr);

    std::lock_guard<std::mutex> lk(g_mtx);
    g_lines.push_back(LogEntry{ now_ms(), level, buf });
    while (g_lines.size() > kKeep) g_lines.pop_front();
}

std::vector<LogEntry> recent_log(size_t max_lines) {
    std::lock_guard<std::mutex> lk(g_mtx);
    std::vector<LogEntry> out;
    const size_t n = g_lines.size() > max_lines ? max_lines : g_lines.size();
    out.reserve(n);
    for (size_t i = g_lines.size() - n; i < g_lines.size(); ++i)
        out.push_back(g_lines[i]);
    return out;
}

} // namespace multisite_relay
