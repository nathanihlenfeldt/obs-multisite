// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin_log.h"

#include <util/platform.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace multisite_obs {

namespace {

// Leaked on purpose, both of them: the remote-control server's connections run
// on detached threads, and one of them may still be asking for the log while
// OBS unloads this module. A destroyed container at that moment is a crash
// nobody would ever be able to diagnose, and a few hundred log lines is a small
// price for never having to.
std::mutex& ring_mutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}

std::deque<PluginLogEntry>& ring() {
    static std::deque<PluginLogEntry>* r = new std::deque<PluginLogEntry>();
    return *r;
}

// A ring, not the whole log: this is for "what just happened", and an event
// that runs for three hours would otherwise grow without limit in a process
// nobody restarts.
constexpr size_t kMaxLines = 400;

} // namespace

void plugin_log_line(int level, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // OBS's own log first: it is the record that survives, and it must not
    // depend on anything this plugin keeps in memory.
    blog(level, "%s", buf);

    PluginLogEntry e;
    e.at_ms = (long long)(os_gettime_ns() / 1000000);
    e.level = level;
    e.text  = buf;

    std::lock_guard<std::mutex> lk(ring_mutex());
    ring().push_back(std::move(e));
    while (ring().size() > kMaxLines) ring().pop_front();
}

std::vector<PluginLogEntry> plugin_log_recent(size_t max_lines) {
    std::lock_guard<std::mutex> lk(ring_mutex());
    std::vector<PluginLogEntry> out;
    const size_t n = ring().size();
    const size_t first = n > max_lines ? n - max_lines : 0;
    out.reserve(n - first);
    for (size_t i = first; i < n; ++i) out.push_back(ring()[i]);
    return out;
}

const char* plugin_log_level_name(int level) {
    switch (level) {
        case LOG_ERROR:   return "error";
        case LOG_WARNING: return "warn";
        case LOG_DEBUG:   return "debug";
        default:          return "info";
    }
}

} // namespace multisite_obs