// SPDX-License-Identifier: GPL-3.0-or-later
//
// digisyn_probe.c — talk to the AES67 card the way its driver actually works,
// before any of it goes near the player.
//
// Why this exists
// ---------------
// The player has never fed this card cleanly, and the log says why: the card's
// ALSA buffer is 8 ms while one decoded frame is about 21 ms, so every frame
// overruns the buffer and ALSA reports an under-run roughly once per frame —
// the "sound has broken up" counter climbing about thirty times a second.
//
// Putting a software queue in front of the ALSA device could not fix that,
// because this driver does not implement a normal playback ring. From the
// vendor's own Digisyn-vSndCard.c:
//
//     .prepare = dummy_pcm_prepare      // `return 0` — a no-op
//     .pointer = dummy_pcm_pointer      // sampleRate/1000 * msIndex % buffer_size
//     // no .copy, no .ack in the ops table
//     substream->runtime->dma_area = Dsp_getBuf_phyToNet(dsp);
//
// msIndex is a millisecond counter that the DigiAes67Proc *daemon* advances.
// So the ALSA "hardware position" is a wall-clock index into a ring of
// one-millisecond slots, and it has nothing to do with how much anyone wrote.
// An eight-slot calendar handed twenty-one milliseconds at a time cannot line
// up; ALSA calls each mismatch an under-run. The buffer is not a queue — it is
// a calendar.
//
// The vendor ships the correct interface in Dsp.h: mmap the device and write
// forty-eight frames straight into the slot for the millisecond you want them
// played. This program does exactly that, and nothing else, so one bench run
// can answer the only question that matters — does writing the calendar
// directly produce sound — before the player is changed at all.
//
// It must run as root: /dev/Digisyn_vSndCard is mode 0600, owner root.
//
//     gcc -O2 -o digisyn_probe digisyn_probe.c -lm
//     sudo ./digisyn_probe            # 1 kHz tone on every channel, until ^C
//     sudo ./digisyn_probe --watch    # print the daemon's clock, write nothing
//
// The calendar is only bufMs deep (8 ms as configured), so the tone is written
// a few milliseconds ahead of the daemon's clock and never further.

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// This mirrors the vendor's Dsp.h. It is the header page of the shared mapping
// the kernel module creates, and it has to match the module's ABI exactly, so
// it is copied rather than reinvented. See DigiAes67KoLib/Dsp.h in the vendor
// package for the original.
#define DSP_DEVICE_NAME "Digisyn_vSndCard"
#define DSP_DEVICE_PATH "/dev/" DSP_DEVICE_NAME

// How big the mapping is depends on the module's build (it is sized for the
// largest card it supports) and on the kernel's page size, so it is not
// computed here: the module writes its own size into mapSize, and that is what
// gets mapped. On the bench box it reports 409600 bytes — a 16384-byte page
// for this header plus two 196608-byte calendars — which is where a Pi running
// 16K pages shows up. Assuming 4K there was the first bug in this program.

typedef struct {
    uint64_t verifyCodeStart;
    uint32_t mapSize;
    uint32_t base_phyToNet;   // offset from here to the playback calendar
    uint32_t base_netToPhy;   // offset from here to the capture calendar
    uint32_t sampleRate;
    uint32_t chNum_phyToNet;
    uint32_t chNum_netToPhy;
    uint32_t bufMs;           // how many 1 ms slots the calendar holds
    volatile uint64_t msIndex;
    uint64_t verifyCodeEnd;
} Dsp_t;

// The module stamps a magic number at both ends of the header so a user-space
// program can tell it is looking at a live mapping and not at random pages.
#define DSP_VERIFY_CODE (*(uint64_t *)DSP_DEVICE_NAME)

static inline uint32_t dsp_ms_to_frame(const Dsp_t *d, uint64_t ms_index)
{
    return (uint32_t)((ms_index % d->bufMs) * (d->sampleRate / 1000));
}

static inline int32_t *dsp_buf_phy_to_net(const Dsp_t *d)
{
    return (int32_t *)((char *)d + d->base_phyToNet);
}

// The slot the daemon will transmit for the given millisecond.
static inline int32_t *dsp_slot(const Dsp_t *d, uint64_t ms_index)
{
    return &dsp_buf_phy_to_net(d)[d->chNum_phyToNet * dsp_ms_to_frame(d, ms_index)];
}

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// Set by map_device() to the size the module asked us to map, so main() can
// unmap the same region it mapped.
static uint32_t g_map_size = 0;

static int map_device(Dsp_t **out)
{
    int fd = open(DSP_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", DSP_DEVICE_PATH, strerror(errno));
        if (errno == EACCES)
            fprintf(stderr, "  it is mode 0600 owned by root — run this as root.\n");
        if (errno == ENOENT)
            fprintf(stderr, "  is the Digisyn_vSndCard module loaded?\n");
        return -1;
    }

    // The header is in the first page, and it tells us how big the whole
    // mapping is; map one page first, read it, then map the rest.
    void *head = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (head == MAP_FAILED) {
        fprintf(stderr, "mmap(header): %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    Dsp_t *d = (Dsp_t *)head;
    if (d->verifyCodeStart != DSP_VERIFY_CODE || d->verifyCodeEnd != DSP_VERIFY_CODE) {
        fprintf(stderr, "the mapping does not carry the vendor's magic number — is the\n"
                        "daemon running? (it fills in the header)\n");
        munmap(head, 4096);
        close(fd);
        return -1;
    }
    // The module's own size is the authority. Sanity-check it against the
    // header rather than a compiled-in constant, so a different page size or a
    // differently built module is not mistaken for a broken vendor package.
    if (d->mapSize < sizeof(Dsp_t) || d->mapSize > (1u << 24)) {
        fprintf(stderr, "the module reports a %u-byte mapping, which cannot hold a\n"
                        "header + calendars — refusing to map it.\n", d->mapSize);
        munmap(head, 4096);
        close(fd);
        return -1;
    }
    uint32_t map_size = d->mapSize;
    munmap(head, 4096);

    void *whole = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (whole == MAP_FAILED) {
        fprintf(stderr, "mmap(%u): %s\n", map_size, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);   // the mapping outlives the descriptor
    g_map_size = map_size;
    *out = (Dsp_t *)whole;
    return 0;
}

static void report(const Dsp_t *d)
{
    printf("card:    %u Hz, %u channels, %u bytes per 1 ms slot (calendar is %u ms deep)\n",
           d->sampleRate, d->chNum_phyToNet,
           d->chNum_phyToNet * (d->sampleRate / 1000) * (int)sizeof(int32_t), d->bufMs);
    printf("clock:   msIndex = %llu\n", (unsigned long long)d->msIndex);
}

static int watch(Dsp_t *d)
{
    unsigned long long start_index = d->msIndex;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_stop) {
        usleep(250000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        unsigned long long ticks = d->msIndex - start_index;
        printf("\rmsIndex = %llu   (+%llu in %.2fs = %.0f ticks/s)   ",
               (unsigned long long)d->msIndex, ticks, dt, ticks / dt);
        fflush(stdout);
    }
    printf("\n");
    return 0;
}

static int play_tone(Dsp_t *d)
{
    if (d->chNum_phyToNet == 0 || d->bufMs == 0) {
        fprintf(stderr, "the daemon has not configured the card yet (%u channels, %u ms).\n"
                        "start DigiAes67Proc first — that is what fills in the header.\n",
                d->chNum_phyToNet, d->bufMs);
        return 1;
    }

    // Written a few milliseconds ahead of the daemon's clock, and — because the
    // calendar has only bufMs slots — never more than that, or we would be
    // overwriting a slot the daemon has not read yet.
    const int lead_ms = 3;
    if (lead_ms >= (int)d->bufMs) {
        fprintf(stderr, "bufMs is %u but this program writes %d ms ahead; the calendar is\n"
                        "too shallow for that. Configure a deeper buffer.\n", d->bufMs, lead_ms);
        return 1;
    }

    const double freq = 1000.0;                      // a tone you cannot mistake for silence
    const double amp = 0.30 * 2147483647.0;          // and cannot mistake for a fault
    const double step = 2.0 * M_PI * freq / d->sampleRate;
    const uint32_t frames_1ms = d->sampleRate / 1000;
    double phase = 0.0;

    uint64_t written = d->msIndex;   // start from now; refill as the clock advances
    unsigned long long slots = 0;

    report(d);
    printf("writing a %.0f Hz tone, %d ms ahead of the clock — ^C to stop\n", freq, lead_ms);

    while (!g_stop) {
        uint64_t now = d->msIndex;

        // If the clock has run away (daemon restarted, box slept), do not try to
        // backfill history — just resume from where it is now.
        if (now > written && now - written > d->bufMs) {
            printf("\nclock jumped by %llu ms — resuming from now\n",
                   (unsigned long long)(now - written));
            written = now;
        }

        while (written < now) {
            ++written;
            int32_t *slot = dsp_slot(d, written + lead_ms);
            for (uint32_t f = 0; f < frames_1ms; ++f) {
                int32_t v = (int32_t)lrint(amp * sin(phase));
                phase += step;
                for (uint32_t c = 0; c < d->chNum_phyToNet; ++c)
                    slot[f * d->chNum_phyToNet + c] = v;
            }
            ++slots;
        }

        printf("\rslots written: %llu   (clock %llu)   ", slots, (unsigned long long)now);
        fflush(stdout);
        usleep(200);   // keep well inside a millisecond so no slot is missed
    }

    printf("\nstopped after %llu slots\n", slots);
    return 0;
}

int main(int argc, char **argv)
{
    int do_watch = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--watch")) {
            do_watch = 1;
        } else if (!strcmp(argv[i], "--help")) {
            printf("usage: %s [--watch]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Dsp_t *d = NULL;
    if (map_device(&d) != 0) return 1;

    int rc = do_watch ? watch(d) : play_tone(d);
    munmap(d, g_map_size);
    return rc;
}
