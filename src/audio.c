/* audio.c — device sinks behind one interface, all found at runtime.
 *
 * The clock deserves a note. We need "what the user is hearing right now", not
 * "what we have handed to the OS". Two approximations are available:
 *
 *   - the device's own timeline (pa_simple_get_latency), which is correct but
 *     only exists for pulse; and
 *   - frames written minus the measured output latency, advanced against
 *     CLOCK_MONOTONIC, which is what we use everywhere else.
 *
 * Both are smoothed by the caller (clock.c), because raw device positions jitter
 * by a few milliseconds and a jittery clock makes video stutter.
 */
#include "audio.h"
#include "util.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* libpulse-simple, dynamically                                       */
/* ------------------------------------------------------------------ */

typedef struct pa_simple pa_simple;
typedef struct {
    int   format;
    int   rate;
    int   channels;
} pa_sample_spec_t;

enum { PA_SAMPLE_S16LE = 3, PA_STREAM_PLAYBACK = 1 };

typedef pa_simple *(*pa_new_fn)(const char *, const char *, int, const char *,
                                const char *, const pa_sample_spec_t *, void *, void *);
typedef int  (*pa_write_fn)(pa_simple *, const void *, size_t, int *);
typedef int  (*pa_drain_fn)(pa_simple *);
typedef void (*pa_free_fn)(pa_simple *);
typedef int  (*pa_latency_fn)(pa_simple *, void *);

typedef struct {
    void *h;
    pa_new_fn     new_;
    pa_write_fn   write_;
    pa_drain_fn   drain_;
    pa_free_fn    free_;
    pa_latency_fn latency_;
} PulseApi;

static PulseApi g_pulse;
static int g_pulse_state = -1;

static bool pulse_load(void)
{
    if (g_pulse_state >= 0) return g_pulse_state == 1;
    static const char *const names[] = {
        "libpulse-simple.so.0", "libpulse-simple.so", NULL
    };
    for (int i = 0; names[i] && !g_pulse.h; i++)
        g_pulse.h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!g_pulse.h) { g_pulse_state = 0; return false; }

    *(void **)(&g_pulse.new_)     = dlsym(g_pulse.h, "pa_simple_new");
    *(void **)(&g_pulse.write_)   = dlsym(g_pulse.h, "pa_simple_write");
    *(void **)(&g_pulse.drain_)   = dlsym(g_pulse.h, "pa_simple_drain");
    *(void **)(&g_pulse.free_)    = dlsym(g_pulse.h, "pa_simple_free");
    *(void **)(&g_pulse.latency_) = dlsym(g_pulse.h, "pa_simple_get_latency");

    g_pulse_state = (g_pulse.new_ && g_pulse.write_ && g_pulse.free_) ? 1 : 0;
    return g_pulse_state == 1;
}

/* ------------------------------------------------------------------ */
/* AudioOut                                                           */
/* ------------------------------------------------------------------ */

typedef enum { SINK_NONE, SINK_PULSE, SINK_FD } SinkKind;

struct AudioOut {
    SinkKind kind;
    int      rate, channels;
    int      latency_ms;

    /* pulse */
    pa_simple *pa;

    /* fd */
    int      fd;

    /* clock */
    int64_t  frames_written;
    int64_t  frames_base;      /* frames written at the last flush   */
    int64_t  base_ns;          /* when the flush happened            */
    int64_t  device_latency_frames;
    bool     dead;
};

static int64_t frames_to_ns(const AudioOut *a, int64_t frames)
{
    if (a->rate <= 0) return 0;
    return frames * 1000000000LL / a->rate;
}

AudioOut *audio_open(int rate, int channels, AudioInfo *info)
{
    if (rate <= 0) rate = 48000;
    if (channels <= 0) channels = 2;

    AudioOut *a = xcalloc(1, sizeof *a);
    a->rate = rate;
    a->channels = channels;
    a->fd = -1;
    a->base_ns = now_ns();

    memset(info, 0, sizeof *info);
    info->rate = rate;
    info->channels = channels;

    /* 1. explicit fd, for tests and unusual environments */
    const char *fd_env = getenv("TERMIPLAYER_AUDIO_FD");
    if (fd_env && *fd_env) {
        a->fd = atoi(fd_env);
        if (a->fd >= 0) {
            a->kind = SINK_FD;
            info->backend = "fd";
            snprintf(info->note, sizeof info->note,
                     "writing PCM to fd %d from TERMIPLAYER_AUDIO_FD", a->fd);
            return a;
        }
    }

    /* 2. pulse (also serves PipeWire) */
    if (pulse_load()) {
        pa_sample_spec_t ss = { PA_SAMPLE_S16LE, (uint32_t)rate, (uint8_t)channels };
        int err = 0;
        a->latency_ms = 40;
        a->pa = g_pulse.new_("termiplayer", "playback", PA_STREAM_PLAYBACK,
                             NULL, "media", &ss, NULL, &err);
        if (a->pa) {
            a->kind = SINK_PULSE;
            info->backend = "pulse";
            info->latency_ms = a->latency_ms;
            snprintf(info->note, sizeof info->note,
                     "PulseAudio/PipeWire via libpulse-simple (40 ms buffer)");
            return a;
        }
        snprintf(info->note, sizeof info->note,
                 "libpulse found but the connection failed (err %d); is a sound server running?", err);
    } else {
        snprintf(info->note, sizeof info->note, "libpulse-simple.so.0 not available");
    }

    /* 3. nothing. Still a valid player: silent, clock from CLOCK_MONOTONIC. */
    a->kind = SINK_NONE;
    info->backend = "none";
    if (info->note[0] == '\0')
        snprintf(info->note, sizeof info->note, "no audio device available; playing silently");
    return a;
}

void audio_close(AudioOut *a)
{
    if (!a) return;
    if (a->kind == SINK_PULSE && a->pa) {
        if (g_pulse.drain_) g_pulse.drain_(a->pa);
        if (g_pulse.free_) g_pulse.free_(a->pa);
    }
    free(a);
}

bool audio_available(const AudioOut *a) { return a && a->kind != SINK_NONE; }

bool audio_write(AudioOut *a, const int16_t *samples, size_t n_samples)
{
    if (!a || !n_samples || a->dead) return a && !a->dead;

    size_t bytes = n_samples * sizeof(int16_t);
    switch (a->kind) {
    case SINK_PULSE: {
        int err = 0;
        if (g_pulse.write_(a->pa, samples, bytes, &err) != 0) {
            a->dead = true;
            return false;
        }
        break;
    }
    case SINK_FD: {
        const char *p = (const char *)samples;
        size_t left = bytes;
        while (left) {
            ssize_t k = write(a->fd, p, left);
            if (k < 0) {
                if (errno == EINTR) continue;
                a->dead = true;
                return false;
            }
            p += k;
            left -= (size_t)k;
        }
        break;
    }
    case SINK_NONE:
    default:
        /* Consume at real-time pace so the clock and the pacing stay honest
         * even with no device: a silent player must not race ahead. */
        sleep_ns(frames_to_ns(a, (int64_t)(n_samples / (size_t)a->channels)));
        break;
    }

    a->frames_written += (int64_t)(n_samples / (size_t)a->channels);
    return true;
}

int64_t audio_frames_written(const AudioOut *a) { return a->frames_written; }

int64_t audio_frames_played(const AudioOut *a_const)
{
    /* The only thing we mutate is a measurement cache, so the cast is safe and
     * keeps the public interface honest: callers must not think this has side
     * effects on playback state. */
    AudioOut *a = (AudioOut *)a_const;
    if (!a) return 0;

    if (a->kind == SINK_PULSE && g_pulse.latency_) {
        /* Trust the device timeline when it is offered, but smooth it against
         * our own accounting: pa_simple_get_latency is measured in microseconds
         * and can step. */
        unsigned long lat_us = (unsigned long)g_pulse.latency_(a->pa, NULL);
        a->device_latency_frames = (int64_t)lat_us * a->rate / 1000000L;
    }

    int64_t elapsed_ns = now_ns() - a->base_ns;
    int64_t elapsed_frames = a->rate * elapsed_ns / 1000000000LL;

    /* Frames we can prove are audible: everything written before the flush,
     * plus what has had time to drain since. */
    int64_t avail = a->frames_written - a->frames_base;
    int64_t played = elapsed_frames < avail ? elapsed_frames : avail;
    if (a->device_latency_frames > 0) {
        int64_t minus = a->device_latency_frames;
        if (played > minus) played -= minus;
    }
    return a->frames_base + played;
}

void audio_flush(AudioOut *a)
{
    if (!a) return;
    /* We cannot reliably flush a pa_simple stream, so we re-base the clock and
     * let the cushion drain naturally: a seek costs at most one buffer of stale
     * audio, and the video clock jumps immediately. */
    a->frames_base = a->frames_written;
    a->base_ns = now_ns();
    a->device_latency_frames = 0;
}

double audio_latency_s(const AudioOut *a)
{
    if (!a || a->rate <= 0) return 0;
    int64_t pending = a->frames_written - audio_frames_played(a);
    if (pending < 0) pending = 0;
    return (double)pending / (double)a->rate;
}

/* ------------------------------------------------------------------ */
/* WAV output (tests, and `--audio-to FILE`)                          */
/* ------------------------------------------------------------------ */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff); p[3] = (uint8_t)((v >> 24) & 0xff);
}
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
}

bool audio_write_wav_header(int fd, int rate, int channels, uint32_t n_frames)
{
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    uint32_t data_bytes = n_frames * (uint32_t)channels * 2u;
    put_u32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_u32(h + 16, 16);
    put_u16(h + 20, 1);
    put_u16(h + 22, (uint16_t)channels);
    put_u32(h + 24, (uint32_t)rate);
    put_u32(h + 28, (uint32_t)(rate * channels * 2));
    put_u16(h + 32, (uint16_t)(channels * 2));
    put_u16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
    return write(fd, h, sizeof h) == (ssize_t)sizeof h;
}
