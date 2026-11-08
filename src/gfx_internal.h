/* gfx_internal.h — backend entry points. Not part of the public surface:
 * gfx.c owns dispatch so backends cannot be called out of order. */
#ifndef TP_GFX_INTERNAL_H
#define TP_GFX_INTERNAL_H

#include "gfx.h"

void gfx_blocks_draw(const TermInfo *ti, TtyWriter *w, const Raster *r,
                     Rect cells, bool truecolor);
void gfx_ascii_draw(const TermInfo *ti, TtyWriter *w, const Raster *r, Rect cells);
void gfx_kitty_draw(const TermInfo *ti, TtyWriter *w, const Raster *r,
                    Rect cells, bool shm);
void gfx_sixel_draw(const TermInfo *ti, TtyWriter *w, const Raster *r, Rect cells);
void gfx_iterm2_draw(const TermInfo *ti, TtyWriter *w, const Raster *r, Rect cells);

#endif /* TP_GFX_INTERNAL_H */
