/* screen.h — a cell grid with diff rendering.
 *
 * Not curses. The reasons are practical:
 *   - we already own the tty and the escape output (term.h), so a second
 *     terminal abstraction would fight it;
 *   - curses is a build dependency, and the project's rule is that `make` needs
 *     nothing but a compiler;
 *   - a diff renderer is ~200 lines and only writes cells that changed, which is
 *     what keeps a text UI at 60 fps over ssh.
 *
 * Every cell holds a Unicode codepoint and 24-bit colours. Backends that cannot
 * do truecolor are handled by the caller choosing a palette (see screen_to256).
 */
#ifndef TP_SCREEN_H
#define TP_SCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "term.h"
#include "util.h"

/* Cell attributes, bit flags. */
enum {
    A_NONE   = 0,
    A_BOLD   = 1u << 0,
    A_DIM    = 1u << 1,
    A_UNDER  = 1u << 2,
    A_REV    = 1u << 3,
};

typedef struct {
    uint32_t ch;      /* Unicode codepoint; 0 means blank (space) */
    uint32_t fg;      /* 0xRRGGBB */
    uint32_t bg;      /* 0xRRGGBB */
    uint8_t  attr;
} Cell;

typedef struct {
    int   w, h;
    Cell *cur;        /* what we want on screen now   */
    Cell *prev;       /* what the terminal is showing */
    bool  force_full; /* next flush repaints everything */
} Screen;

void screen_init(Screen *s, int w, int h);
void screen_free(Screen *s);
bool screen_resize(Screen *s, int w, int h);

/* Clear the desired buffer to spaces with the given colours. */
void screen_clear(Screen *s, uint32_t fg, uint32_t bg);

/* Write a UTF-8 string, clipped to `maxw` cells. Returns cells written. */
int screen_text(Screen *s, int x, int y, int maxw, const char *utf8,
                uint32_t fg, uint32_t bg, uint8_t attr);

/* Write a string padded with spaces to exactly `width` cells. Returns width. */
int screen_text_pad(Screen *s, int x, int y, int width, const char *utf8,
                    uint32_t fg, uint32_t bg, uint8_t attr);

/* Right-align a string in `width` cells. */
void screen_text_right(Screen *s, int x, int y, int width, const char *utf8,
                       uint32_t fg, uint32_t bg, uint8_t attr);

void screen_fill(Screen *s, int x, int y, int w, int h,
                 uint32_t fg, uint32_t bg, uint8_t attr);
void screen_hline(Screen *s, int x, int y, int w, uint32_t fg, uint32_t bg);
void screen_vline(Screen *s, int x, int y, int h, uint32_t fg, uint32_t bg);
void screen_box(Screen *s, int x, int y, int w, int h, uint32_t fg, uint32_t bg);

/* A box with a title in the top border. */
void screen_box_titled(Screen *s, int x, int y, int w, int h, const char *title,
                       uint32_t fg, uint32_t bg);

/* Emit the difference between prev and cur into `w`, then adopt cur as prev.
 * The caller must have wrapped this in ttyw_begin_sync/end_sync. */
void screen_flush(Screen *s, TtyWriter *w, bool truecolor, bool full_repaint);

/* Force the next flush to repaint everything (after a resize or mode change). */
void screen_invalidate(Screen *s);

/* Horizontal progress bar drawn in cells: "████░░░░" style. */
void screen_bar(Screen *s, int x, int y, int width, double frac,
                uint32_t fill_fg, uint32_t track_fg, uint32_t bg);

/* Display width of a UTF-8 string in cells (assumes 1 cell per codepoint, which
 * holds for the Latin text this UI uses; CJK width handling is a TODO). */
int utf8_width(const char *s);
/* Truncate UTF-8 to at most `maxw` cells, appending "…" when cut.
 * Writes into out (cap bytes) and returns cells used. */
int utf8_ellipsize(char *out, size_t cap, const char *s, int maxw);

#endif /* TP_SCREEN_H */
