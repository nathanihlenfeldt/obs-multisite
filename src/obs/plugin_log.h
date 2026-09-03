#pragma once
#include <obs-module.h>
#define PLOG "[multisite] "
#define mlog_info(f, ...)  blog(LOG_INFO,    PLOG f, ##__VA_ARGS__)
#define mlog_warn(f, ...)  blog(LOG_WARNING, PLOG f, ##__VA_ARGS__)
#define mlog_error(f, ...) blog(LOG_ERROR,   PLOG f, ##__VA_ARGS__)
#define mlog_debug(f, ...) blog(LOG_DEBUG,   PLOG f, ##__VA_ARGS__)

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
