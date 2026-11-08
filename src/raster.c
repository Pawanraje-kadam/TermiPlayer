#include "raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void raster_init(Raster *r) { r->w = r->h = 0; r->px = NULL; }

void raster_free(Raster *r)
{
    free(r->px);
    raster_init(r);
}

bool raster_alloc(Raster *r, int w, int h)
{
    if (w <= 0 || h <= 0) { raster_free(r); return false; }
    if (r->w == w && r->h == h) return true;             /* reuse */
    uint8_t *px = realloc(r->px, (size_t)w * (size_t)h * 3u);
    if (!px) return false;
    r->px = px;
    r->w = w;
    r->h = h;
    return true;
}

void raster_clear(Raster *r)
{
    if (r->px) memset(r->px, 0, (size_t)r->w * (size_t)r->h * 3u);
}

void raster_scale_nn(const Raster *src, Raster *dst)
{
    if (!src->px || !dst->px || src->w <= 0 || src->h <= 0) return;
    const int sw = src->w, sh = src->h, dw = dst->w, dh = dst->h;
    if (sw == dw && sh == dh) {
        memcpy(dst->px, src->px, (size_t)sw * (size_t)sh * 3u);
        return;
    }

    /* Precompute the source column for each destination column: this turns a
     * per-pixel multiply+divide into two table lookups, which matters when we
     * do this at 30 fps. */
    int *colmap = malloc((size_t)dw * sizeof *colmap);
    if (!colmap) return;
    for (int x = 0; x < dw; x++)
        colmap[x] = (int)((int64_t)x * sw / dw);

    for (int y = 0; y < dh; y++) {
        int sy = (int)((int64_t)y * sh / dh);
        const uint8_t *srow = src->px + (size_t)sy * sw * 3u;
        uint8_t *drow = dst->px + (size_t)y * dw * 3u;
        for (int x = 0; x < dw; x++) {
            const uint8_t *s = srow + (size_t)colmap[x] * 3u;
            drow[x * 3 + 0] = s[0];
            drow[x * 3 + 1] = s[1];
            drow[x * 3 + 2] = s[2];
        }
    }
    free(colmap);
}

void raster_blit(const Raster *src, int sx, int sy, int sw, int sh,
                 Raster *dst, int dx, int dy)
{
    if (!src->px || !dst->px) return;

    /* clip the source region */
    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;
    /* clip the destination */
    if (dx < 0) { sw += dx; sx -= dx; dx = 0; }
    if (dy < 0) { sh += dy; sy -= dy; dy = 0; }
    if (dx + sw > dst->w) sw = dst->w - dx;
    if (dy + sh > dst->h) sh = dst->h - dy;
    if (sw <= 0 || sh <= 0) return;

    for (int y = 0; y < sh; y++) {
        memcpy(dst->px + ((size_t)(dy + y) * dst->w + dx) * 3u,
               src->px + ((size_t)(sy + y) * src->w + sx) * 3u,
               (size_t)sw * 3u);
    }
}

void raster_fill(Raster *r, int x, int y, int w, int h,
                 uint8_t cr, uint8_t cg, uint8_t cb)
{
    if (!r->px) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > r->w) w = r->w - x;
    if (y + h > r->h) h = r->h - y;
    if (w <= 0 || h <= 0) return;

    for (int yy = 0; yy < h; yy++) {
        uint8_t *row = r->px + ((size_t)(y + yy) * r->w + x) * 3u;
        for (int xx = 0; xx < w; xx++) {
            row[xx * 3 + 0] = cr;
            row[xx * 3 + 1] = cg;
            row[xx * 3 + 2] = cb;
        }
    }
}

void raster_rect(Raster *r, int x, int y, int w, int h,
                 uint8_t cr, uint8_t cg, uint8_t cb)
{
    raster_fill(r, x, y, w, 1, cr, cg, cb);
    raster_fill(r, x, y + h - 1, w, 1, cr, cg, cb);
    raster_fill(r, x, y, 1, h, cr, cg, cb);
    raster_fill(r, x + w - 1, y, 1, h, cr, cg, cb);
}

RasterStats raster_stats(const Raster *r)
{
    RasterStats st = { 0, 0 };
    if (!r->px || r->w < 2 || r->h < 2) return st;

    /* Sample a bounded number of rows/columns so this stays O(1)-ish
     * regardless of resolution: it runs once per frame. */
    int stepx = r->w / 64; if (stepx < 1) stepx = 1;
    int stepy = r->h / 48; if (stepy < 1) stepy = 1;

    double sum = 0;
    long n = 0;
    for (int y = 0; y < r->h; y += stepy) {
        const uint8_t *row = r->px + (size_t)y * r->w * 3u;
        for (int x = stepx; x < r->w; x += stepx) {
            const uint8_t *a = row + (size_t)x * 3u;
            const uint8_t *b = a - 3;
            sum += fabs((double)a[0] - b[0]) + fabs((double)a[1] - b[1]) +
                   fabs((double)a[2] - b[2]);
            n += 3;
        }
    }
    if (!n) return st;
    st.mean_abs_delta = sum / (double)n;
    /* 255/4 per channel sits around "very busy"; saturate at 1. */
    st.entropy_hint = st.mean_abs_delta / 64.0;
    if (st.entropy_hint > 1.0) st.entropy_hint = 1.0;
    return st;
}
