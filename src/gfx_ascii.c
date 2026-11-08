/* gfx_ascii.c — the floor of the ladder. Must never fail.
 *
 * One character per cell, luminance mapped through a ramp. Uses 256-colour
 * foreground when the terminal has colour at all, plain text otherwise, so it
 * works on TERM=dumb.
 */
#include "gfx_internal.h"

/* Dark to light. Chosen so adjacent glyphs differ by a similar ink amount. */
static const char RAMP[] = " .:-=+*#%@";
#define RAMP_N (sizeof(RAMP) - 1)

static inline int luma(const uint8_t *p)
{
    /* integer Rec.601: cheap and good enough for ASCII */
    return (299 * p[0] + 587 * p[1] + 114 * p[2]) / 1000;
}

void gfx_ascii_draw(const TermInfo *ti, TtyWriter *w, const Raster *r, Rect cells)
{
    if (!r->px || r->w < 1 || r->h < 1) return;

    Buf *b = &w->b;
    int cols = r->w < cells.w ? r->w : cells.w;
    int rows = r->h < cells.h ? r->h : cells.h;
    bool colour = ti->truecolor || ti->is_tty;

    buf_puts(b, "\x1b[0m");
    for (int y = 0; y < rows; y++) {
        gfx_cursor(w, cells.y + y + 1, cells.x + 1);
        const uint8_t *row = r->px + (size_t)y * r->w * 3u;
        int last = -1;
        for (int x = 0; x < cols; x++) {
            const uint8_t *p = row + (size_t)x * 3u;
            int l = luma(p);
            int idx = (l * (int)(RAMP_N - 1)) / 255;
            if (idx < 0) idx = 0;
            if (idx > (int)RAMP_N - 1) idx = (int)RAMP_N - 1;

            if (colour && l != last) {
                /* index into the grey ramp for a smooth luminance gradient */
                int gi = 232 + (l * 23) / 255;
                buf_printf(b, "\x1b[38;5;%dm", gi);
                last = l;
            }
            buf_putc(b, RAMP[idx]);
        }
    }
    buf_puts(b, "\x1b[0m");
}
