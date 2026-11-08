#include "screen.h"
#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* UTF-8                                                              */
/* ------------------------------------------------------------------ */

/* Decode one codepoint. Returns bytes consumed (>= 1); 0 on invalid. */
static size_t utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) { *cp = 0; return 0; }
    if (p[0] < 0x80) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
              (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

static size_t utf8_emit(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int utf8_width(const char *s)
{
    int n = 0;
    uint32_t cp;
    size_t k;
    while (s && *s && (k = utf8_next(s, &cp))) {
        (void)cp;
        s += k;
        n++;
    }
    return n;
}

int utf8_ellipsize(char *out, size_t cap, const char *s, int maxw)
{
    if (!out || cap == 0) return 0;
    if (maxw <= 0) { out[0] = '\0'; return 0; }

    if (utf8_width(s) <= maxw) {
        snprintf(out, cap, "%s", s ? s : "");
        return utf8_width(out);
    }
    if (maxw <= 2) {
        /* too narrow for an ellipsis */
        out[0] = '\0';
        int w = 0;
        uint32_t cp; size_t k;
        const char *p = s;
        while (*p && w < maxw && (k = utf8_next(p, &cp))) {
            char tmp[8];
            size_t n = utf8_emit(cp, tmp);
            if (w * 1 + n + 1 > cap) break;
            memcpy(out + strlen(out), tmp, n);
            out[strlen(out) + n] = '\0';
            p += k;
            w++;
        }
        return w;
    }
    int keep = maxw - 1;            /* room for the ellipsis */
    size_t o = 0;
    int w = 0;
    const char *p = s;
    uint32_t cp; size_t k;
    while (*p && w < keep && (k = utf8_next(p, &cp)) && o + 5 < cap) {
        char tmp[8];
        size_t n = utf8_emit(cp, tmp);
        memcpy(out + o, tmp, n);
        o += n;
        p += k;
        w++;
    }
    static const char ELL[] = "\xe2\x80\xa6";   /* U+2026 */
    if (o + sizeof ELL <= cap) {
        memcpy(out + o, ELL, sizeof ELL - 1);
        o += sizeof ELL - 1;
    }
    out[o] = '\0';
    return w + 1;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

void screen_init(Screen *s, int w, int h)
{
    memset(s, 0, sizeof *s);
    screen_resize(s, w, h);
    s->force_full = true;
}

void screen_free(Screen *s)
{
    free(s->cur);
    free(s->prev);
    memset(s, 0, sizeof *s);
}

bool screen_resize(Screen *s, int w, int h)
{
    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
    size_t n = (size_t)w * (size_t)h;
    Cell *cur = calloc(n, sizeof *cur);
    Cell *prev = calloc(n, sizeof *prev);
    if (!cur || !prev) { free(cur); free(prev); return false; }
    for (size_t i = 0; i < n; i++) {
        cur[i].ch = ' ';
        prev[i].ch = 0;            /* 0 forces a repaint of every cell */
    }
    free(s->cur); free(s->prev);
    s->cur = cur; s->prev = prev;
    s->w = w; s->h = h;
    s->force_full = true;
    return true;
}

void screen_invalidate(Screen *s) { s->force_full = true; }

void screen_clear(Screen *s, uint32_t fg, uint32_t bg)
{
    size_t n = (size_t)s->w * (size_t)s->h;
    for (size_t i = 0; i < n; i++) {
        s->cur[i].ch = ' ';
        s->cur[i].fg = fg;
        s->cur[i].bg = bg;
        s->cur[i].attr = A_NONE;
    }
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static inline Cell *at(Screen *s, int x, int y)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return NULL;
    return &s->cur[(size_t)y * s->w + x];
}

static inline void put(Screen *s, int x, int y, uint32_t ch,
                       uint32_t fg, uint32_t bg, uint8_t attr)
{
    Cell *c = at(s, x, y);
    if (!c) return;
    c->ch = ch ? ch : ' ';
    c->fg = fg;
    c->bg = bg;
    c->attr = attr;
}

int screen_text(Screen *s, int x, int y, int maxw, const char *utf8,
                uint32_t fg, uint32_t bg, uint8_t attr)
{
    if (!utf8 || maxw <= 0) return 0;
    int w = 0;
    const char *p = utf8;
    uint32_t cp; size_t k;
    while (*p && w < maxw && (k = utf8_next(p, &cp))) {
        put(s, x + w, y, cp, fg, bg, attr);
        p += k;
        w++;
    }
    return w;
}

int screen_text_pad(Screen *s, int x, int y, int width, const char *utf8,
                    uint32_t fg, uint32_t bg, uint8_t attr)
{
    int w = screen_text(s, x, y, width, utf8, fg, bg, attr);
    for (int i = w; i < width; i++) put(s, x + i, y, ' ', fg, bg, attr);
    return width;
}

void screen_text_right(Screen *s, int x, int y, int width, const char *utf8,
                       uint32_t fg, uint32_t bg, uint8_t attr)
{
    /* Right-align WITHOUT padding the gap with spaces. The obvious
     * implementation (pad left, then write) silently erases anything already
     * drawn earlier on the same row -- which wiped every library title behind
     * its duration column. The screen is cleared each frame, so there is
     * nothing to erase and no reason to touch the cells to the left. */
    if (width <= 0) return;
    char tmp[1024];
    int sw = utf8_width(utf8);
    const char *str = utf8;
    if (sw > width) {
        utf8_ellipsize(tmp, sizeof tmp, utf8, width);
        str = tmp;
        sw = utf8_width(tmp);
    }
    if (sw > width) sw = width;
    screen_text(s, x + width - sw, y, sw, str, fg, bg, attr);
}

void screen_fill(Screen *s, int x, int y, int w, int h,
                 uint32_t fg, uint32_t bg, uint8_t attr)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            put(s, x + xx, y + yy, ' ', fg, bg, attr);
}

void screen_hline(Screen *s, int x, int y, int w, uint32_t fg, uint32_t bg)
{
    for (int i = 0; i < w; i++) put(s, x + i, y, 0x2500, fg, bg, A_NONE);
}

void screen_vline(Screen *s, int x, int y, int h, uint32_t fg, uint32_t bg)
{
    for (int i = 0; i < h; i++) put(s, x, y + i, 0x2502, fg, bg, A_NONE);
}

void screen_box(Screen *s, int x, int y, int w, int h, uint32_t fg, uint32_t bg)
{
    if (w < 2 || h < 2) return;
    /* corners, then edges */
    put(s, x, y, 0x250C, fg, bg, A_NONE);                 /* ┌ */
    put(s, x + w - 1, y, 0x2510, fg, bg, A_NONE);         /* ┐ */
    put(s, x, y + h - 1, 0x2514, fg, bg, A_NONE);         /* └ */
    put(s, x + w - 1, y + h - 1, 0x2518, fg, bg, A_NONE); /* ┘ */
    for (int i = 1; i < w - 1; i++) {
        put(s, x + i, y, 0x2500, fg, bg, A_NONE);
        put(s, x + i, y + h - 1, 0x2500, fg, bg, A_NONE);
    }
    for (int i = 1; i < h - 1; i++) {
        put(s, x, y + i, 0x2502, fg, bg, A_NONE);
        put(s, x + w - 1, y + i, 0x2502, fg, bg, A_NONE);
    }
}

void screen_box_titled(Screen *s, int x, int y, int w, int h, const char *title,
                       uint32_t fg, uint32_t bg)
{
    screen_box(s, x, y, w, h, fg, bg);
    if (title && *title && w > 4) {
        put(s, x + 2, y, ' ', fg, bg, A_NONE);
        int tw = screen_text(s, x + 3, y, w - 6, title, fg, bg, A_BOLD);
        put(s, x + 3 + tw, y, ' ', fg, bg, A_NONE);
    }
}

void screen_bar(Screen *s, int x, int y, int width, double frac,
                uint32_t fill_fg, uint32_t track_fg, uint32_t bg)
{
    if (width <= 0) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    /* eighth-block resolution, so a 20-cell bar is not 5% steps */
    static const uint32_t EIGHTHS[9] = {
        0x0020, 0x258F, 0x258E, 0x258D, 0x258C, 0x258B, 0x258A, 0x2589, 0x2588
    };
    double cells = frac * (double)width;
    int full = (int)cells;
    double rem = cells - (double)full;
    int e = (int)(rem * 8.0 + 0.5);
    if (e > 8) { e = 0; full++; }

    for (int i = 0; i < width; i++) {
        if (i < full) {
            put(s, x + i, y, 0x2588, fill_fg, bg, A_NONE);
        } else if (i == full && e > 0) {
            put(s, x + i, y, EIGHTHS[e], fill_fg, bg, A_NONE);
        } else {
            put(s, x + i, y, 0x2591, track_fg, bg, A_NONE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* diff flush                                                         */
/* ------------------------------------------------------------------ */

static void emit_sgr(Buf *b, const Cell *c, bool truecolor)
{
    buf_puts(b, "\x1b[");
    bool first = true;
    if (c->attr & A_BOLD) {
        buf_puts(b, "1"); first = false;
    }
    if (c->attr & A_DIM) {
        if (!first) buf_putc(b, ';');
        buf_puts(b, "2"); first = false;
    }
    if (c->attr & A_UNDER) {
        if (!first) buf_putc(b, ';');
        buf_puts(b, "4"); first = false;
    }
    if (c->attr & A_REV) {
        if (!first) buf_putc(b, ';');
        buf_puts(b, "7"); first = false;
    }
    if (!first) buf_putc(b, ';');

    if (truecolor) {
        /* One SGR for both colours: the 'm' terminates the whole sequence, so
         * it must appear only at the end. */
        buf_printf(b, "38;2;%u;%u;%u;48;2;%u;%u;%um",
                   (c->fg >> 16) & 0xFF, (c->fg >> 8) & 0xFF, c->fg & 0xFF,
                   (c->bg >> 16) & 0xFF, (c->bg >> 8) & 0xFF, c->bg & 0xFF);
    } else {
        /* 256-colour fallback: map to the cube + grey ramp */
        int fr = (int)((c->fg >> 16) & 0xFF), fgc = (int)((c->fg >> 8) & 0xFF),
            fb = (int)(c->fg & 0xFF);
        int br = (int)((c->bg >> 16) & 0xFF), bgc = (int)((c->bg >> 8) & 0xFF),
            bb = (int)(c->bg & 0xFF);
        int fi = 16 + 36 * (fr * 5 / 255) + 6 * (fgc * 5 / 255) + (fb * 5 / 255);
        int bi = 16 + 36 * (br * 5 / 255) + 6 * (bgc * 5 / 255) + (bb * 5 / 255);
        buf_printf(b, "38;5;%d;48;5;%dm", fi, bi);
    }
    buf_putc(b, 'm');
}

void screen_flush(Screen *s, TtyWriter *w, bool truecolor, bool full_repaint)
{
    Buf *b = &w->b;
    bool full = full_repaint || s->force_full;

    Cell last;
    memset(&last, 0xFF, sizeof last);   /* impossible value */
    bool have_last = false;

    for (int y = 0; y < s->h; y++) {
        int x = 0;
        while (x < s->w) {
            Cell *c = &s->cur[(size_t)y * s->w + x];
            Cell *p = &s->prev[(size_t)y * s->w + x];

            bool changed = full || c->ch != p->ch || c->fg != p->fg ||
                           c->bg != p->bg || c->attr != p->attr;
            if (!changed) { x++; continue; }

            /* cursor to the start of this run */
            gfx_cursor(w, y + 1, x + 1);

            while (x < s->w) {
                c = &s->cur[(size_t)y * s->w + x];
                p = &s->prev[(size_t)y * s->w + x];
                changed = full || c->ch != p->ch || c->fg != p->fg ||
                          c->bg != p->bg || c->attr != p->attr;
                if (!changed) break;

                if (!have_last || c->fg != last.fg || c->bg != last.bg ||
                    c->attr != last.attr) {
                    emit_sgr(b, c, truecolor);
                    last = *c;
                    have_last = true;
                }
                char tmp[8];
                size_t n = utf8_emit(c->ch ? c->ch : ' ', tmp);
                buf_append(b, tmp, n);

                *p = *c;              /* adopt: the terminal now shows this */
                x++;
            }
        }
    }
    buf_puts(b, "\x1b[0m");
    s->force_full = false;
}
