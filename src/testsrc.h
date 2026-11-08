/* testsrc.h — a synthetic source, so `player testsrc` proves the whole render
 * path on any machine with no media files and no ffmpeg.
 *
 * It is deliberately diagnostic rather than pretty:
 *   - a moving vertical bar (shows frame pacing and drop/repeat visually)
 *   - a horizontal grey ramp (shows palette banding per backend)
 *   - a colour grid (shows channel swap and truecolor support)
 *   - a per-frame counter rendered as a bar pattern (shows actual frame count)
 *   - a 1px border (shows raster size and clipping)
 */
#ifndef TP_TESTSRC_H
#define TP_TESTSRC_H

#include <stdint.h>

#include "raster.h"

/* Render frame number `n` of `fps` into `r`, which must already be allocated.
 * `phase_s` is seconds since start, used for smooth motion. */
void testsrc_render(Raster *r, int n, double phase_s);

/* Render a static "no signal" frame, used when a stream ends unexpectedly. */
void testsrc_render_static(Raster *r);

#endif /* TP_TESTSRC_H */
