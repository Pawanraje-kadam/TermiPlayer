#include "gfx_internal.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* base64                                                             */
/* ------------------------------------------------------------------ */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_size(size_t n, bool pad)
{
    size_t q = (n / 3) * 4;
    size_t rem = n % 3;
    if (rem == 1) q += pad ? 4 : 2;
    else if (rem == 2) q += pad ? 4 : 3;
    return q;
}

size_t b64_encode(const uint8_t *in, size_t n, char *out, size_t out_cap, bool pad)
{
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        if (o + 4 > out_cap) return o;
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    size_t rem = n - i;
    if (rem == 1) {
        if (o + (pad ? 4u : 2u) > out_cap) return o;
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        if (pad) { out[o++] = '='; out[o++] = '='; }
    } else if (rem == 2) {
        if (o + (pad ? 4u : 3u) > out_cap) return o;
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        if (pad) out[o++] = '=';
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* zlib via dlopen: zero build-time dependency                        */
/* ------------------------------------------------------------------ */

typedef int (*compress2_fn)(unsigned char *, unsigned long *, const unsigned char *,
                            unsigned long, int);

static compress2_fn g_compress2;
static int g_zlib_state = -1;   /* -1 unknown, 0 absent, 1 present */

bool zlib_available(void)
{
    if (g_zlib_state >= 0) return g_zlib_state == 1;

    /* Try the common sonames; musl and some distros differ. */
    static const char *const names[] = {
        "libz.so.1", "libz.so", "libz.dylib", "libz.1.dylib", NULL
    };
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
    if (!h) { g_zlib_state = 0; return false; }

    *(void **)(&g_compress2) = dlsym(h, "compress2");
    g_zlib_state = g_compress2 ? 1 : 0;
    return g_zlib_state == 1;
}

size_t zlib_compress(const void *in, size_t n, void *out, size_t out_cap, int level)
{
    if (!zlib_available()) return 0;
    unsigned long dl = (unsigned long)out_cap;
    int rc = g_compress2((unsigned char *)out, &dl,
                         (const unsigned char *)in, (unsigned long)n, level);
    if (rc != 0) return 0;
    return (size_t)dl;
}

/* ------------------------------------------------------------------ */
/* cursor helpers                                                     */
/* ------------------------------------------------------------------ */

void gfx_cursor(TtyWriter *w, int row, int col)
{
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    buf_printf(&w->b, "\x1b[%d;%dH", row, col);
}

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

void gfx_raster_size(BackendId id, const TermInfo *ti, Rect cells, int *w, int *h)
{
    int cw = ti->cell_w > 0 ? ti->cell_w : 8;
    int ch = ti->cell_h > 0 ? ti->cell_h : 16;

    switch (id) {
    case BK_KITTY_SHM:
    case BK_KITTY:
    case BK_ITERM2:
    case BK_SIXEL:
        /* real pixels: exactly the pixel box the cells occupy */
        *w = cells.w * cw;
        *h = cells.h * ch;
        break;

    case BK_BLOCKS24:
    case BK_BLOCKS256:
        /* 1x2 pixels per cell (half blocks) */
        *w = cells.w;
        *h = cells.h * 2;
        break;

    case BK_ASCII:
    default:
        *w = cells.w;
        *h = cells.h;
        break;
    }

    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    /* guard against absurd geometry from a broken ioctl */
    if (*w > 4096) *w = 4096;
    if (*h > 4096) *h = 4096;
}

void gfx_draw(BackendId id, const TermInfo *ti, TtyWriter *w,
              const Raster *r, Rect cells)
{
    switch (id) {
    case BK_KITTY_SHM: gfx_kitty_draw(ti, w, r, cells, true);  break;
    case BK_KITTY:     gfx_kitty_draw(ti, w, r, cells, false); break;
    case BK_ITERM2:    gfx_iterm2_draw(ti, w, r, cells);       break;
    case BK_SIXEL:     gfx_sixel_draw(ti, w, r, cells);        break;
    case BK_BLOCKS256: gfx_blocks_draw(ti, w, r, cells, false); break;
    case BK_ASCII:     gfx_ascii_draw(ti, w, r, cells);        break;
    case BK_BLOCKS24:
    default:           gfx_blocks_draw(ti, w, r, cells, true);  break;
    }
}

void gfx_clear(BackendId id, TtyWriter *w, Rect cells)
{
    switch (id) {
    case BK_KITTY_SHM:
    case BK_KITTY:
        /* delete our image id, then blank the cells so nothing stale shows */
        buf_puts(&w->b, "\x1b_Ga=d,d=I,i=1\x1b\\");
        break;
    case BK_ITERM2:
        break;   /* inline images scroll away; cells get overwritten */
    default:
        break;
    }
    (void)cells;
}

void gfx_shutdown(BackendId id, TtyWriter *w)
{
    switch (id) {
    case BK_KITTY_SHM:
    case BK_KITTY:
        buf_puts(&w->b, "\x1b_Ga=d,d=A\x1b\\");
        break;
    default:
        break;
    }
}

bool gfx_needs_clear(BackendId id)
{
    return id == BK_SIXEL || id == BK_ITERM2;
}
