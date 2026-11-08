#include "testsrc.h"
#include "util.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

/* 5x7 bitmap font, only the glyphs we need for a frame counter. Each glyph is
 * 7 rows of 5 bits, MSB left. Keeping this here rather than pulling in a font
 * library is the difference between "compiles anywhere" and "needs deps". */
static const uint8_t FONT_DIGITS[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },  /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },  /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },  /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },  /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },  /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },  /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },  /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },  /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },  /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },  /* 9 */
};

static void draw_glyph(Raster *r, int x0, int y0, int scale, const uint8_t rows[7],
                       uint8_t cr, uint8_t cg, uint8_t cb)
{
    for (int gy = 0; gy < 7; gy++) {
        uint8_t bits = rows[gy];
        for (int gx = 0; gx < 5; gx++) {
            if (!(bits & (1u << (4 - gx)))) continue;
            raster_fill(r, x0 + gx * scale, y0 + gy * scale, scale, scale, cr, cg, cb);
        }
    }
}

void testsrc_render(Raster *r, int n, double phase_s)
{
    if (!r->px) return;
    const int w = r->w, h = r->h;

    /* 1. background: vertical gradient so palette banding is obvious */
    for (int y = 0; y < h; y++) {
        uint8_t v = (uint8_t)(y * 255 / (h > 1 ? h - 1 : 1));
        raster_fill(r, 0, y, w, 1, v, v, v);
    }

    /* 2. colour grid across the top third: pure R/G/B/C/M/Y/W blocks */
    static const uint8_t GRID[8][3] = {
        { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 0, 255, 255 },
        { 255, 0, 255 }, { 255, 255, 0 }, { 255, 255, 255 }, { 32, 32, 32 },
    };
    int gh = h / 3;
    for (int i = 0; i < 8; i++) {
        int x = i * w / 8;
        int ww = (i + 1) * w / 8 - x;
        raster_fill(r, x, 0, ww, gh, GRID[i][0], GRID[i][1], GRID[i][2]);
    }

    /* 3. moving vertical bar: shows pacing. Sweeps once per 2 seconds. */
    int barw = w / 24; if (barw < 1) barw = 1;
    int bx = (int)(fmod(phase_s * 0.5, 1.0) * (double)w);
    raster_fill(r, bx, gh, barw, h - gh, 255, 255, 255);

    /* 4. diagonal line: shows aspect-ratio errors and shear */
    for (int i = 0; i < w; i++) {
        int y = gh + (i * (h - gh)) / (w > 0 ? w : 1);
        raster_fill(r, i, y, 1, 1, 255, 128, 0);
    }

    /* 5. frame counter, bottom-left, scaled to the raster */
    int scale = h / 48; if (scale < 1) scale = 1;
    char num[16];
    snprintf(num, sizeof num, "%d", n);
    int cx = 2;
    int cy = h - 7 * scale - 2;
    if (cy > gh) {
        for (const char *p = num; *p; p++) {
            if (*p < '0' || *p > '9') continue;
            draw_glyph(r, cx, cy, scale, FONT_DIGITS[*p - '0'], 255, 255, 0);
            cx += 6 * scale;
        }
    }

    /* 6. 1px border: shows clipping and raster size */
    raster_rect(r, 0, 0, w, h, 0, 255, 0);
}

void testsrc_render_static(Raster *r)
{
    if (!r->px) return;
    /* classic "no signal" bars */
    static const uint8_t BARS[8][3] = {
        { 192, 192, 192 }, { 192, 192, 0 }, { 0, 192, 192 }, { 0, 192, 0 },
        { 192, 0, 192 }, { 192, 0, 0 }, { 0, 0, 192 }, { 0, 0, 0 },
    };
    for (int i = 0; i < 8; i++) {
        int x = i * r->w / 8;
        int ww = (i + 1) * r->w / 8 - x;
        raster_fill(r, x, 0, ww, r->h, BARS[i][0], BARS[i][1], BARS[i][2]);
    }
}
