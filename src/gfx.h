/* gfx.h — graphics backends.
 *
 * Contract: every backend is handed a Raster already sized to the pixels it
 * should occupy, plus the cell rectangle it lives in, and appends escape bytes
 * to a TtyWriter. No backend touches the framebuffer, the clock or the decoder.
 *
 * Backends must satisfy two properties, both tested:
 *   - emit only sequences they close (no dangling APC/OSC/DCS)
 *   - never move the cursor as a side effect (the screen layer owns that)
 */
#ifndef TP_GFX_H
#define TP_GFX_H

#include <stdbool.h>

#include "raster.h"
#include "term.h"
#include "util.h"

typedef struct { int x, y, w, h; } Rect;   /* in cells */

/* Pixel dimensions a backend wants for a given cell rectangle. */
void gfx_raster_size(BackendId id, const TermInfo *ti, Rect cells, int *w, int *h);

/* Draw `r` into the cell rectangle `cells`. The writer must already be in sync
 * mode; the backend may emit long sequences. */
void gfx_draw(BackendId id, const TermInfo *ti, TtyWriter *w,
              const Raster *r, Rect cells);

/* Erase whatever the backend previously drew in `cells`. */
void gfx_clear(BackendId id, TtyWriter *w, Rect cells);

/* Called once on exit: delete stored images, etc. */
void gfx_shutdown(BackendId id, TtyWriter *w);

/* True if the backend needs an explicit clear before each new frame at the
 * same position (blocks/ascii overwrite; sixel/kitty place images). */
bool gfx_needs_clear(BackendId id);

/* ---- shared helpers used by more than one backend ---- */

/* Move cursor to (row, col), 1-based, into w. */
void gfx_cursor(TtyWriter *w, int row, int col);

/* Base64 (no padding option). Returns bytes written. */
size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t out_cap, bool pad);

/* Current size of the base64 output for n input bytes. */
size_t b64_size(size_t n, bool pad);

/* zlib compress2 via dlopen("libz.so.1"). Returns compressed size, or 0 if
 * zlib is unavailable or compression failed. `level` 1..9. */
size_t zlib_compress(const void *in, size_t n, void *out, size_t out_cap, int level);

/* True if a working zlib was found at runtime. */
bool zlib_available(void);

#endif /* TP_GFX_H */
