/* audio.h — output device, and the audio clock that drives video.
 *
 * Backends are resolved at runtime, best first:
 *
 *   pulse   libpulse-simple via dlopen -- PipeWire and PulseAudio both serve it,
 *           which covers the overwhelming majority of Linux desktops. No build
 *           dependency, no link flag.
 *   alsa    libasound via dlopen, if present.
 *   fd      a pre-opened fd from $TERMIPLAYER_AUDIO_FD (useful inside a sandbox,
 *           and how the test suite feeds a WAV file).
 *   none    no device. The player keeps working: the clock falls back to
 *           CLOCK_MONOTONIC, so video still plays at the right speed, silently.
 *
 * This is what makes "audio works with zero system dependencies" true rather
 * than aspirational: absence of every backend is a supported state.
 */
#ifndef TP_AUDIO_H
#define TP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct AudioOut AudioOut;

typedef struct {
    const char *backend;      /* "pulse", "alsa", "fd", "none" */
    int  rate;
    int  channels;
    int  latency_ms;          /* device buffer we asked for */
    char note[200];           /* why it landed here / what failed */
} AudioInfo;

/* Attempt to open a device. Never returns NULL: an "unavailable" sink is a
 * valid AudioOut that reports backend "none". */
AudioOut *audio_open(int rate, int channels, AudioInfo *info);
void      audio_close(AudioOut *a);

bool audio_available(const AudioOut *a);

/* Queue interleaved s16 samples. Blocks when the device is full (that is the
 * A/V pacing mechanism: audio is the bottleneck, by design). Returns false if
 * the sink is dead. */
bool audio_write(AudioOut *a, const int16_t *samples, size_t n_samples);

/* How many frames have actually been *heard* since open, measured against the
 * device's own clock where possible. This is the master clock. */
int64_t audio_frames_played(const AudioOut *a);

/* Frames accepted by the device (>= played; the difference is the cushion). */
int64_t audio_frames_written(const AudioOut *a);

/* Drop everything queued and reset the clock base (used after a seek). */
void audio_flush(AudioOut *a);

/* Seconds of buffered audio the device holds right now. */
double audio_latency_s(const AudioOut *a);

/* ---- writing a WAV, used by tests and by `player --record-audio` ---- */
bool audio_write_wav_header(int fd, int rate, int channels, uint32_t n_frames);

#endif /* TP_AUDIO_H */
