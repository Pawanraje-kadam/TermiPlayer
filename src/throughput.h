/* throughput.h — the mechanism no other terminal player has.
 *
 * THE MEASUREMENT THIS EXISTS FOR (spikes/RESULTS.md §1, Finding 2):
 *
 *   At a 10 MB/s pty, a 320x90 truecolor half-block raster tops out at ~9 fps.
 *   160x50 gives 32 fps, 80x24 gives 134 fps. Sixel is ~70x cheaper in bytes at
 *   the same geometry; local kitty-shm is a flat 64 bytes/frame.
 *
 * So "pick a resolution at startup and hope" is not a viable strategy: the
 * achievable raster depends on the link, the terminal's parser, the content and
 * the frame rate, and none of those are known until playback starts.
 *
 * WHAT THIS DOES
 *
 *   - measures real bytes/second leaving the tty writer (EMA, not a guess)
 *   - estimates the bytes each candidate raster would need, from the *measured*
 *     bytes per cell of the current backend and the frame's entropy
 *   - picks the largest raster that sustains the target frame rate with headroom
 *   - applies hysteresis so quality does not oscillate between two rasters
 *   - can switch backend class when the gap is large (blocks -> 256-colour)
 *
 * The output is advisory: the session asks for a raster size before decoding
 * each frame and honours the answer.
 */
#ifndef TP_THROUGHPUT_H
#define TP_THROUGHPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "term.h"

typedef struct {
    /* inputs */
    int     target_fps;        /* what we are trying to sustain (e.g. 30) */
    int     cols, rows;        /* the cell box we may draw into */
    int     cell_w, cell_h;    /* pixels per cell, for pixel backends */

    /* measured */
    double  bytes_per_sec;     /* EMA of real throughput */
    double  bytes_per_cell;    /* EMA of the current backend's cost */
    double  entropy;           /* 0..1 content busyness, from raster_stats */
    double  decode_ms;         /* EMA of decode+scale time per frame */

    /* output */
    int     raster_w, raster_h;
    int     fps_cap;           /* we may lower this instead of resolution */
    bool    downgrade_backend; /* true when blocks24 should become blocks256 */
    const char *why;           /* human-readable reason, for --stats */
} TpAdvice;

typedef struct Throughput Throughput;

Throughput *tp_new(int cols, int rows, int cell_w, int cell_h, int target_fps);
void        tp_free(Throughput *t);

/* Called after each frame with what actually happened.
 * `frame_dt_s` is the wall-clock interval the frame took, which is what makes
 * the bytes/second measurement meaningful: bytes per syscall would report
 * gigabytes on any pipe that drains quickly. Pass 0 to let the controller
 * measure the interval itself from now_ns(). */
void tp_observe(Throughput *t, long bytes_written, double frame_dt_s,
                double decode_ms, double entropy, BackendId backend);

/* Ask for the raster to use for the next frame. */
TpAdvice tp_advise(Throughput *t, BackendId backend);

/* The cell box changed (resize): reset the byte-per-cell estimate, which was
 * measured at the old geometry. */
void tp_resize(Throughput *t, int cols, int rows, int cell_w, int cell_h);

/* Force a raster (used by --size and by tests). */
void tp_force_raster(Throughput *t, int w, int h);

#endif /* TP_THROUGHPUT_H */
