/* term.h — the terminal: raw mode, geometry, capability detection, output.
 *
 * Three jobs, one file, because they all need the same tty fd and the same
 * "is this actually a terminal" answer:
 *
 *   1. term_open/close   : raw mode, alternate screen, cursor, mouse
 *   2. term_probe        : geometry in cells AND pixels, backend ladder
 *   3. TtyWriter         : buffered, resize-safe escape output with BSU/ESU
 *
 * Everything degrades: on TERM=dumb or a non-tty, probe() still returns a
 * usable geometry and the backend ladder ends at ASCII.
 */
#ifndef TP_TERM_H
#define TP_TERM_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

/* ---- backends -------------------------------------------------------- */

typedef enum {
    BK_KITTY_SHM = 0,   /* kitty graphics, shared-memory transfer (local)   */
    BK_KITTY,           /* kitty graphics, direct transfer (zlib if we can)  */
    BK_ITERM2,          /* OSC 1337 inline images                            */
    BK_SIXEL,           /* DEC sixel                                         */
    BK_BLOCKS24,        /* truecolor half-blocks                             */
    BK_BLOCKS256,       /* 256-colour half-blocks                            */
    BK_ASCII,           /* always works                                      */
    BK__COUNT
} BackendId;

const char *backend_name(BackendId id);
/* Parse "kitty", "sixel", "blocks", "ascii", ... NULL if unknown. */
bool backend_parse(const char *s, BackendId *out);

/* Why a backend was chosen or rejected -- shown by `player doctor`. */
typedef struct {
    BackendId id;
    bool      available;
    char      reason[160];
} BackendVerdict;

typedef struct {
    /* cells */
    int  cols, rows;
    /* pixels; 0 means "unknown". Populated by CSI 16t/14t or ioctl when the
     * terminal reports pixel geometry (most do not over ssh). */
    int  cell_w, cell_h;
    int  px_w, px_h;

    bool is_tty;
    bool truecolor;
    bool kitty_protocol;
    bool iterm2_images;
    bool sixel;
    bool in_tmux;
    bool in_screen;
    bool in_zellij;

    char term[128];
    char term_program[128];
    char colorterm[64];

    BackendId chosen;
    BackendVerdict verdicts[BK__COUNT];
    int  n_verdicts;
} TermInfo;

/* ---- lifecycle ------------------------------------------------------- */

typedef struct Term Term;

/* Opens /dev/tty (never stdin, so `player file | tee` still works) and puts it
 * in raw mode on the alternate screen. Returns NULL when no tty is available:
 * callers must fall back to a "no terminal" mode rather than failing. */
Term *term_open(bool use_alt_screen);
void  term_close(Term *t);

int  term_fd(const Term *t);
/* Frame the geometry, capability set and backend choice. Cached; re-run on
 * resize. Explicit `force` backend overrides detection. */
void term_probe(Term *t, const char *force_backend);
const TermInfo *term_info(const Term *t);

/* Refresh cols/rows from the kernel (cheap, no escape round-trip).
 * Returns true if the size actually changed. */
bool term_refresh_size(Term *t);
/* True if a SIGWINCH has arrived since the last call. */
bool term_take_resized(Term *t);

/* ---- input ----------------------------------------------------------- */

typedef enum {
    KEY_NONE = 0,
    KEY_CHAR,        /* .ch set; may be multi-byte UTF-8 in .utf8 */
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    KEY_ENTER, KEY_ESC, KEY_TAB, KEY_BACKSPACE, KEY_DELETE,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_RESIZE,
} KeyKind;

typedef struct {
    KeyKind kind;
    char    ch[8];      /* UTF-8, NUL-terminated, when kind == KEY_CHAR */
    bool    ctrl;
    bool    alt;
    bool    shift;
} Key;

/* Poll for a key for up to timeout_ms (0 = non-blocking, -1 = forever). */
bool term_read_key(Term *t, Key *k, int timeout_ms);

/* ---- output ---------------------------------------------------------- */

/* One frame's worth of escape bytes. Owns a buffer that grows and is reused;
 * never shrinks, so steady-state playback does no allocation. */
typedef struct {
    int   fd;
    Buf   b;
    bool  sync_begin;      /* DEC 2026 in effect */
    int64_t bytes_written; /* since last consume */
    double  write_s;
} TtyWriter;

void ttyw_init(TtyWriter *w, int fd);
void ttyw_free(TtyWriter *w);
/* Begin a synchronized-output block (BSU). Safe to nest; only the outermost
 * pair is emitted. */
void ttyw_begin_sync(TtyWriter *w);
void ttyw_end_sync(TtyWriter *w);
/* Flush the buffered frame to the fd. Returns bytes written, -1 on error.
 * The writer brackets the write in BSU/ESU if the caller did not. */
long ttyw_flush(TtyWriter *w);
/* Rate measurement for the throughput controller. Resets the counters. */
double ttyw_take_bytes_per_sec(TtyWriter *w);

/* Convenience: write raw bytes directly (used for teardown sequences). */
long tty_write_all(int fd, const void *data, size_t n);

/* Query helpers used by the prober. Each has a hard timeout, because a
 * terminal that does not understand a query simply never replies. */
bool term_query_cell_pixels(int fd, int *cell_w, int *cell_h);
bool term_query_window_pixels(int fd, int *px_w, int *px_h);
bool term_query_kitty(int fd);
bool term_query_sixel(int fd);

#endif /* TP_TERM_H */
