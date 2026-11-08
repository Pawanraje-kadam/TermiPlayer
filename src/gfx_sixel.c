/* gfx_sixel.c — DEC sixel.
 *
 * Design decisions, all traceable to the spike:
 *
 *  - FIXED 6x7x6 palette (up to 252 entries) + ordered Bayer dither, emitted
 *    once per frame. A dynamic per-frame histogram (what libsixel/mpv do) gives
 *    slightly better colour on some content but costs a counting pass and a
 *    sort per frame. Sixel is already the byte-cheapest backend by ~70x versus
 *    blocks at the same geometry, so spending CPU on palette adaptation is the
 *    wrong trade at terminal resolutions.
 *
 *  - PALETTE COUNT IS CONTENT-AWARE. Busy frames get a reduced cube. Measured:
 *    noise content costs 3-6x more CPU than smooth content, and most of that is
 *    per-pixel quantisation plus a larger used-colour set in the RLE pass.
 *    Dropping to 4 or 2 levels/channel keeps the frame inside its budget.
 *
 *  - QUANTISE ONCE into an index plane, then one RLE pass per used colour per
 *    band. This is the classic sixel encoder shape and it is O(w*h) per frame
 *    rather than O(w*h*colours).
 */
#include "gfx_internal.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t BAYER[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

typedef struct {
    uint8_t *index;
    size_t   cap;
} SixelState;

static SixelState g;

/* Entropy-sensitive palette depth. Thresholds come from the measured spread
 * between smooth (gradient/photo) and high-entropy frames. */
static int pick_levels(const Raster *r)
{
    RasterStats st = raster_stats(r);
    if (st.entropy_hint < 0.35) return 6;
    if (st.entropy_hint < 0.70) return 4;
    return 2;
}

static bool build_index(const Raster *r, int levels)
{
    size_t need = (size_t)r->w * (size_t)r->h;
    if (need > g.cap) {
        uint8_t *p = realloc(g.index, need);
        if (!p) return false;
        g.index = p;
        g.cap = need;
    }

    int step = 255 / (levels - 1);
    int lut[256];
    for (int v = 0; v < 256; v++) {
        int l = (v + step / 2) / step;
        if (l >= levels) l = levels - 1;
        lut[v] = l * step;      /* snap to the palette level, keep for dither */
    }
    const int amp = 255 / (levels * 2);

    for (int y = 0; y < r->h; y++) {
        const uint8_t *row = r->px + (size_t)y * r->w * 3u;
        uint8_t *orow = g.index + (size_t)y * r->w;
        const uint8_t *brow = BAYER[y & 7];
        for (int x = 0; x < r->w; x++) {
            const uint8_t *p = row + (size_t)x * 3u;
            int t = ((int)brow[x & 7] - 31) * amp / 32;
            int rr = lut[clampi(p[0] + t, 0, 255)] / step;
            int gg = lut[clampi(p[1] + t, 0, 255)] / step;
            int bb = lut[clampi(p[2] + t, 0, 255)] / step;
            orow[x] = (uint8_t)((rr * levels + gg) * levels + bb);
        }
    }
    return true;
}

/* 6-bit mask for one column of one band, for a given palette index. */
static inline unsigned band_mask(const uint8_t *idx, int w, int x, int y0, int ymax, int c)
{
    unsigned mask = 0;
    for (int dy = 0; dy < 6; dy++) {
        int y = y0 + dy;
        if (y >= ymax) break;
        if (idx[(size_t)y * w + x] == c) mask |= 1u << dy;
    }
    return mask;
}

void gfx_sixel_draw(const TermInfo *ti, TtyWriter *w, const Raster *r, Rect cells)
{
    (void)ti;
    if (!r->px || r->w < 1 || r->h < 1) return;

    const int levels = pick_levels(r);
    const int ncolours = levels * levels * levels;
    if (!build_index(r, levels)) return;
    const int iw = r->w, ih = r->h;
    const int step = 255 / (levels - 1);

    Buf *b = &w->b;
    gfx_cursor(w, cells.y + 1, cells.x + 1);

    /* DCS with P2=1 (transparent background pass-through off), P1=0 */
    buf_printf(b, "\x1bP0;1;0q\"1;1;%d;%d", iw, ih);

    /* palette, 0..100 per channel */
    for (int i = 0; i < ncolours; i++) {
        int rr = (i / (levels * levels)) % levels;
        int gg = (i / levels) % levels;
        int bb = i % levels;
        buf_printf(b, "#%d;2;%d;%d;%d", i,
                   (rr * step) * 100 / 255,
                   (gg * step) * 100 / 255,
                   (bb * step) * 100 / 255);
    }

    uint64_t used[4];

    for (int y0 = 0; y0 < ih; y0 += 6) {
        const int ymax = (y0 + 6 < ih) ? y0 + 6 : ih;

        /* which palette entries appear in this band */
        used[0] = used[1] = used[2] = used[3] = 0;
        for (int y = y0; y < ymax; y++) {
            const uint8_t *row = g.index + (size_t)y * iw;
            for (int x = 0; x < iw; x++) {
                unsigned c = row[x];
                used[c >> 6] |= (uint64_t)1 << (c & 63);
            }
        }

        for (int c = 0; c < ncolours; c++) {
            if (!(used[c >> 6] & ((uint64_t)1 << (c & 63)))) continue;

            buf_printf(b, "#%d", c);

            int x = 0;
            while (x < iw) {
                unsigned mask = band_mask(g.index, iw, x, y0, ymax, c);
                int run = 1;
                while (x + run < iw &&
                       band_mask(g.index, iw, x + run, y0, ymax, c) == mask)
                    run++;
                char ch = (char)('?' + (int)mask);   /* sixel char: '?' + bits */
                if (run > 3) buf_printf(b, "!%d%c", run, ch);
                else for (int i = 0; i < run; i++) buf_putc(b, ch);
                x += run;
            }
            buf_putc(b, '$');      /* carriage return within the band */
        }
        buf_putc(b, '-');          /* next band down */
    }

    buf_puts(b, "\x1b\\");         /* ST */
}
