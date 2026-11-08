/* gfx_blocks.c — the universal fallback, and the backend that matters over ssh.
 *
 * WHAT THE SPIKE TAUGHT US (spikes/RESULTS.md):
 *
 *   37 bytes/cell for truecolor: "\x1b[38;2;R;G;Bm" + "\x1b[48;2;R;G;Bm" + "▀".
 *   Delta-encoding neighbouring cells buys 0.0-0.1% on real footage, because
 *   every cell in a video frame is already a unique colour. So we do NOT ship a
 *   delta encoder for truecolor. What we do ship, because it is free:
 *
 *     1. SGR only on change. Colour state carries across cells AND across rows
 *        (resetting per row is what doubles a naive encoder's output).
 *     2. run-length over identical neighbours: letterbox bars, flat skies and
 *        subtitle boxes collapse to one glyph plus a repeat.
 *     3. an indexed 256-colour mode at 19 B/cell instead of 37 (measured 51%),
 *        which the throughput controller switches to when the link is starved.
 *
 * Cursor positioning is explicit per row rather than "\r\n", so rendering a
 * rectangle narrower than the terminal cannot auto-wrap and skip a line.
 */
#include "gfx_internal.h"

#include <string.h>

#define GLYPH_UPPER_HALF "\xe2\x96\x80"   /* U+2580 */
#define GLYPH_FULL       "\xe2\x96\x88"   /* U+2588: used when both px match */

/* ------------------------------------------------------------------ */
/* 256-colour palette conversion                                      */
/* ------------------------------------------------------------------ */

/* xterm cube: levels 0,95,135,175,215,255. Round without a search. */
static inline int cube_level(int v)
{
    if (v < 48) return 0;
    if (v < 115) return 1;
    return (v - 35) / 40;
}

static inline uint8_t to256(uint8_t r, uint8_t g, uint8_t b)
{
    int ri = cube_level(r), gi = cube_level(g), bi = cube_level(b);
    int rr = ri ? 55 + ri * 40 : 0;
    int gg = gi ? 55 + gi * 40 : 0;
    int bb = bi ? 55 + bi * 40 : 0;
    int cube_err = (r - rr) * (r - rr) + (g - gg) * (g - gg) + (b - bb) * (b - bb);

    /* The 24-step grey ramp (232..255) is far finer than the cube for greys,
     * and greys are most of what film letterboxing and UI chrome produce. */
    int avg = (r + g + b) / 3;
    if (avg < 8) return 16;                 /* pure black: cube entry 16 */
    int gi24 = (avg - 8) / 10;
    if (gi24 > 23) gi24 = 23;
    int gv = 8 + gi24 * 10;
    int grey_err = (r - gv) * (r - gv) + (g - gv) * (g - gv) + (b - gv) * (b - gv);

    if (grey_err < cube_err) return (uint8_t)(232 + gi24);
    return (uint8_t)(16 + 36 * ri + 6 * gi + bi);
}

/* ------------------------------------------------------------------ */
/* run-length state                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    Buf    *b;
    int     pend;          /* glyphs buffered, not yet emitted */
    const char *pend_glyph;
    uint32_t cur_fg, cur_bg;
} Paint;

#define UNSET 0xFFFFFFFFu

static void paint_flush_run(Paint *p)
{
    if (!p->pend) return;
    const char *g = p->pend_glyph ? p->pend_glyph : GLYPH_UPPER_HALF;
    size_t glen = strlen(g);
    buf_reserve(p->b, (size_t)p->pend * glen);
    for (int i = 0; i < p->pend; i++) memcpy(p->b->p + p->b->len + (size_t)i * glen, g, glen);
    p->b->len += (size_t)p->pend * glen;
    p->pend = 0;
    p->pend_glyph = NULL;
}

static inline void paint_glyph(Paint *p, const char *glyph)
{
    if (p->pend && p->pend_glyph != glyph) paint_flush_run(p);
    p->pend_glyph = glyph;
    p->pend++;
}

/* ------------------------------------------------------------------ */
/* truecolor                                                          */
/* ------------------------------------------------------------------ */

static void draw_truecolor(const Raster *r, int cols, int rows, Rect cells, TtyWriter *w)
{
    Buf *b = &w->b;
    Paint p = { b, 0, NULL, UNSET, UNSET };

    /* Reset once at the top; after this we only ever emit deltas. */
    buf_puts(b, "\x1b[0m");

    for (int cy = 0; cy < rows; cy++) {
        paint_flush_run(&p);
        gfx_cursor(w, cells.y + cy + 1, cells.x + 1);

        int y0 = cy * 2, y1 = y0 + 1;
        bool have_bottom = y1 < r->h;

        for (int x = 0; x < cols; x++) {
            const uint8_t *t = r->px + ((size_t)y0 * r->w + x) * 3u;
            const uint8_t *u = have_bottom ? r->px + ((size_t)y1 * r->w + x) * 3u : t;

            uint32_t fv = ((uint32_t)t[0] << 16) | ((uint32_t)t[1] << 8) | t[2];
            uint32_t bv = ((uint32_t)u[0] << 16) | ((uint32_t)u[1] << 8) | u[2];

            if (fv != p.cur_fg) {
                paint_flush_run(&p);
                buf_printf(b, "\x1b[38;2;%u;%u;%um", t[0], t[1], t[2]);
                p.cur_fg = fv;
            }
            if (bv != p.cur_bg) {
                paint_flush_run(&p);
                buf_printf(b, "\x1b[48;2;%u;%u;%um", u[0], u[1], u[2]);
                p.cur_bg = bv;
            }
            /* When both pixels are identical the full block carries the same
             * picture with one colour statement fewer on the next cell. */
            paint_glyph(&p, fv == bv ? GLYPH_FULL : GLYPH_UPPER_HALF);
        }
    }
    paint_flush_run(&p);
    buf_puts(b, "\x1b[0m");
}

/* ------------------------------------------------------------------ */
/* 256-colour                                                         */
/* ------------------------------------------------------------------ */

static void draw_256(const Raster *r, int cols, int rows, Rect cells, TtyWriter *w)
{
    Buf *b = &w->b;
    Paint p = { b, 0, NULL, UNSET, UNSET };

    buf_puts(b, "\x1b[0m");

    for (int cy = 0; cy < rows; cy++) {
        paint_flush_run(&p);
        gfx_cursor(w, cells.y + cy + 1, cells.x + 1);

        int y0 = cy * 2, y1 = y0 + 1;
        bool have_bottom = y1 < r->h;

        for (int x = 0; x < cols; x++) {
            const uint8_t *t = r->px + ((size_t)y0 * r->w + x) * 3u;
            const uint8_t *u = have_bottom ? r->px + ((size_t)y1 * r->w + x) * 3u : t;

            uint32_t fv = to256(t[0], t[1], t[2]);
            uint32_t bv = to256(u[0], u[1], u[2]);

            if (fv != p.cur_fg) {
                paint_flush_run(&p);
                buf_printf(b, "\x1b[38;5;%um", (unsigned)fv);
                p.cur_fg = fv;
            }
            if (bv != p.cur_bg) {
                paint_flush_run(&p);
                buf_printf(b, "\x1b[48;5;%um", (unsigned)bv);
                p.cur_bg = bv;
            }
            paint_glyph(&p, fv == bv ? GLYPH_FULL : GLYPH_UPPER_HALF);
        }
    }
    paint_flush_run(&p);
    buf_puts(b, "\x1b[0m");
}

/* ------------------------------------------------------------------ */

void gfx_blocks_draw(const TermInfo *ti, TtyWriter *w, const Raster *r,
                     Rect cells, bool truecolor)
{
    (void)ti;
    if (!r->px || r->w < 1 || r->h < 1 || cells.w < 1 || cells.h < 1) return;

    int cols = r->w < cells.w ? r->w : cells.w;
    int rows = (r->h + 1) / 2;
    if (rows > cells.h) rows = cells.h;

    if (truecolor) draw_truecolor(r, cols, rows, cells, w);
    else           draw_256(r, cols, rows, cells, w);
}
