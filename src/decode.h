/* decode.h — media decoding without a compile-time FFmpeg dependency.
 *
 * WHY THIS SHAPE
 *
 * The plan called for `ffmpeg-the-third` linked into the binary. That is the
 * right long-term answer, but it makes the build require FFmpeg headers and
 * shared libraries, and it is what makes terminal players painful to install.
 *
 * This implementation drives an `ffmpeg` *process* over two pipes:
 *
 *      ffmpeg ... -f rawvideo -pix_fmt rgb24 -      (video frames on fd)
 *      ffmpeg ... -f s16le  -ac 2 -ar 48000 -       (PCM on fd)
 *
 * It costs one extra process and one extra copy, and buys three things that
 * matter more at this stage:
 *
 *   1. zero build-time deps -- `make` needs nothing but a C compiler;
 *   2. every codec, container and hardware decoder FFmpeg has, for free;
 *   3. a clean seam. `DecodeSource` is an interface: swapping in linked
 *      libavcodec later touches one file and no callers (see docs/ROADMAP.md).
 *
 * The seam is deliberate, not an accident of convenience.
 */
#ifndef TP_DECODE_H
#define TP_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "raster.h"

typedef struct {
    /* stream info, filled by decode_open */
    double  duration;        /* seconds, 0 if unknown  */
    int     src_w, src_h;    /* video dimensions        */
    double  src_fps;
    bool    has_video;
    bool    has_audio;
    int     audio_rate;
    int     audio_channels;

    char    ffmpeg_path[512];   /* which binary we ended up using */
} MediaInfo;

typedef struct DecodeSource DecodeSource;

/* Locate an ffmpeg binary: $TERMIPLAYER_FFMPEG, then PATH, then a short list of
 * well-known locations. Returns NULL if nothing usable is found; *why explains. */
const char *decode_find_ffmpeg(char *buf, size_t cap, const char **why);

/* Open `path` (a file or URL ffmpeg understands). `want_video`/`want_audio`
 * let an audio-only or video-only session skip a whole pipe. */
DecodeSource *decode_open(const char *path, bool want_video, bool want_audio,
                          MediaInfo *out, char *err, size_t errcap);
void decode_close(DecodeSource *d);

/* Read one full RGB24 frame into `r` (allocated/resized as needed).
 * Returns 1 on success, 0 at end of stream, -1 on error. */
int decode_read_video(DecodeSource *d, Raster *r);

/* Read interleaved s16 stereo samples. Returns frames*2 shorts read,
 * 0 at end of stream, -1 on error. */
ssize_t decode_read_audio(DecodeSource *d, int16_t *buf, size_t max_samples);

/* Stop the ffmpeg process early (e.g. user quit). Safe to call twice. */
void decode_kill(DecodeSource *d);

/* Bytes per pixel of the video pipe (fixed at 3 for rgb24). */
#define DECODE_BPP 3

/* The audio pipe is always resampled to this rate,and stereo, so the clock
 * arithmetic is a single constant everywhere. */
#define DECODE_AUDIO_RATE 48000

#endif /* TP_DECODE_H */
