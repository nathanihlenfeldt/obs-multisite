// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <obs-module.h>

#include <string>
#include <vector>

#define PLOG "[multisite] "

namespace multisite_obs {

// Every line this plugin writes to OBS's log is also kept here, newest last.
//
// The remote-control pages need it: an operator standing at the back of the
// room with a tablet cannot open the log file on a machine they are not sitting
// at, and "why is nothing happening" is exactly the question they are asking.
// The appliance and the relay keep the same ring for the same reason.
struct PluginLogEntry {
    long long   at_ms = 0;
    int         level = LOG_INFO;
    std::string text;
};

// Writes to OBS's log AND keeps the line. Called through the mlog_* macros, so
// nothing else in the plugin has to know this exists.
void plugin_log_line(int level, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

std::vector<PluginLogEntry> plugin_log_recent(size_t max_lines = 200);
const char* plugin_log_level_name(int level);

} // namespace multisite_obs

#define mlog_info(f, ...)  ::multisite_obs::plugin_log_line(LOG_INFO,    PLOG f, ##__VA_ARGS__)
#define mlog_warn(f, ...)  ::multisite_obs::plugin_log_line(LOG_WARNING, PLOG f, ##__VA_ARGS__)
#define mlog_error(f, ...) ::multisite_obs::plugin_log_line(LOG_ERROR,   PLOG f, ##__VA_ARGS__)
#define mlog_debug(f, ...) ::multisite_obs::plugin_log_line(LOG_DEBUG,   PLOG f, ##__VA_ARGS__)

#include <media-io/audio-io.h>

// Channel count -> OBS speaker layout. In packed multi-channel mode the layout
// is only a channel COUNT carrier; the positional meanings (FL/FR/LFE/...) are
// irrelevant because arbitrary content is packed into each channel. Declaring
// the wrong count is not cosmetic: OBS would take only the channels the layout
// implies and silently drop the rest.
static inline enum speaker_layout ms_layout_for_channels(int channels) {
    switch (channels) {
        case 1:  return SPEAKERS_MONO;
        case 2:  return SPEAKERS_STEREO;
        case 3:  return SPEAKERS_2POINT1;
        case 4:  return SPEAKERS_4POINT0;
        case 5:  return SPEAKERS_4POINT1;
        case 6:  return SPEAKERS_5POINT1;
        case 8:  return SPEAKERS_7POINT1;
        default: return SPEAKERS_UNKNOWN;
    }
}
