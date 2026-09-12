// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stub of exactly the ALSA surface `src/appliance/alsa_output.cpp` uses, so
// that file can be checked on a machine with no alsa-lib — which is every
// developer machine here, and most of CI.
//
// Why it exists
// -------------
// `alsa_output.cpp` is compiled only where ALSA and libasound2-dev are present,
// so on a laptop it is not compiled at all: a rename, a wrong type or a member
// that no longer exists would be found on a Raspberry Pi rather than in the
// editor. Declaring just the surface that file calls is enough for the compiler
// to check it. The procedure is written up under "Checking the appliance's code
// on a machine that cannot build it" in docs/DEVELOPER.md.
//
//   c++ -std=c++17 -fsyntax-only -Wall -Wextra -Itests/stubs \
//       -Isrc -Isrc/appliance -Isrc/core src/appliance/alsa_output.cpp
//
// It is a syntax check and nothing else: there are no definitions here, so this
// cannot be linked, and it must never end up on the include path of a real
// build — it would shadow the real <alsa/asoundlib.h> and a sound card would
// stop working with no compiler error to explain it.
//
// The signatures match the real header. Where they matter, CI compiling the
// same file against libasound2-dev is the authority, and this is the early
// warning.
//
// Keep it to what the file uses: a declaration left behind after the code that
// needed it was removed is noise, and it hides the fact that nothing calls it.
#pragma once
#include <stddef.h>
#include <alloca.h>

typedef struct _snd_pcm snd_pcm_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;
typedef int snd_pcm_stream_t;
typedef int snd_pcm_access_t;
typedef int snd_pcm_format_t;
typedef struct _snd_pcm_hw_params { int _opaque; } snd_pcm_hw_params_t;
typedef struct _snd_pcm_sw_params { int _opaque; } snd_pcm_sw_params_t;
typedef struct _snd_pcm_format_mask { int _opaque; } snd_pcm_format_mask_t;

#define SND_PCM_STREAM_PLAYBACK 0
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE 2
#define SND_PCM_FORMAT_S32_LE 10
#define SND_PCM_FORMAT_FLOAT_LE 14
#define SND_PCM_FORMAT_LAST 52

#define snd_pcm_hw_params_alloca(ptr) \
    *(ptr) = (snd_pcm_hw_params_t*)alloca(sizeof(snd_pcm_hw_params_t))
#define snd_pcm_sw_params_alloca(ptr) \
    *(ptr) = (snd_pcm_sw_params_t*)alloca(sizeof(snd_pcm_sw_params_t))
#define snd_pcm_format_mask_alloca(ptr) \
    *(ptr) = (snd_pcm_format_mask_t*)alloca(sizeof(snd_pcm_format_mask_t))

int  snd_pcm_open(snd_pcm_t**, const char*, snd_pcm_stream_t, int);
int  snd_pcm_close(snd_pcm_t*);
int  snd_pcm_prepare(snd_pcm_t*);
int  snd_pcm_drop(snd_pcm_t*);
int  snd_pcm_recover(snd_pcm_t*, int, int);
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t*, const void*, snd_pcm_uframes_t);
snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t*);
int  snd_pcm_delay(snd_pcm_t*, snd_pcm_sframes_t*);
int  snd_pcm_get_params(snd_pcm_t*, snd_pcm_uframes_t*, snd_pcm_uframes_t*);

int  snd_pcm_hw_params_any(snd_pcm_t*, snd_pcm_hw_params_t*);
int  snd_pcm_hw_params_set_access(snd_pcm_t*, snd_pcm_hw_params_t*, snd_pcm_access_t);
int  snd_pcm_hw_params_test_format(snd_pcm_t*, snd_pcm_hw_params_t*, snd_pcm_format_t);
int  snd_pcm_hw_params_set_format(snd_pcm_t*, snd_pcm_hw_params_t*, snd_pcm_format_t);
int  snd_pcm_hw_params_get_format_mask(snd_pcm_hw_params_t*, snd_pcm_format_mask_t*);
int  snd_pcm_hw_params_set_channels(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int);
int  snd_pcm_hw_params_set_channels_near(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int*);
int  snd_pcm_hw_params_set_rate_near(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int*, int*);
int  snd_pcm_hw_params_set_buffer_time_near(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int*, int*);
int  snd_pcm_hw_params_set_period_time_near(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int*, int*);
int  snd_pcm_hw_params(snd_pcm_t*, snd_pcm_hw_params_t*);

int  snd_pcm_sw_params_current(snd_pcm_t*, snd_pcm_sw_params_t*);
int  snd_pcm_sw_params_set_start_threshold(snd_pcm_t*, snd_pcm_sw_params_t*, snd_pcm_uframes_t);
int  snd_pcm_sw_params_set_avail_min(snd_pcm_t*, snd_pcm_sw_params_t*, snd_pcm_uframes_t);
int  snd_pcm_sw_params(snd_pcm_t*, snd_pcm_sw_params_t*);

int  snd_pcm_format_mask_test(const snd_pcm_format_mask_t*, snd_pcm_format_t);
const char* snd_pcm_format_name(snd_pcm_format_t);
const char* snd_strerror(int);

int  snd_device_name_hint(int, const char*, void***);
char* snd_device_name_get_hint(const void*, const char*);
int  snd_device_name_free_hint(void**);
