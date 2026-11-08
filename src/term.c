#include "term.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* global resize flag (set from the signal handler)                   */
/* ------------------------------------------------------------------ */

#include <signal.h>
static volatile sig_atomic_t g_resized = 1;

static void on_winch(int sig) { (void)sig; g_resized = 1; }

struct Term {
    int  fd;
    bool own_tty;
    bool alt_screen;
    struct termios saved;
    bool saved_ok;
    TermInfo info;
};

/* ------------------------------------------------------------------ */
/* backend names                                                      */
/* ------------------------------------------------------------------ */

static const char *const k_backend_names[BK__COUNT] = {
    "kitty-shm", "kitty", "iterm2", "sixel", "blocks", "blocks256", "ascii",
};

const char *backend_name(BackendId id)
{
    if ((int)id < 0 || id >= BK__COUNT) return "?";
    return k_backend_names[id];
}

bool backend_parse(const char *s, BackendId *out)
{
    if (!s) return false;
    if (!strcmp(s, "kitty"))     { *out = BK_KITTY;    return true; }
    if (!strcmp(s, "kitty-shm")) { *out = BK_KITTY_SHM; return true; }
    if (!strcmp(s, "iterm2"))    { *out = BK_ITERM2;   return true; }
    if (!strcmp(s, "sixel"))     { *out = BK_SIXEL;    return true; }
    if (!strcmp(s, "blocks"))    { *out = BK_BLOCKS24; return true; }
    if (!strcmp(s, "blocks256")) { *out = BK_BLOCKS256; return true; }
    if (!strcmp(s, "ascii"))     { *out = BK_ASCII;    return true; }
    if (!strcmp(s, "auto"))      { *out = BK__COUNT;   return true; }
    return false;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

static void set_raw(struct termios *t)
{
    t->c_iflag &= (tcflag_t)~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                              IGNCR | ICRNL | IXON | IXOFF);
    t->c_oflag &= (tcflag_t)~OPOST;
    t->c_lflag &= (tcflag_t)~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= (tcflag_t)~(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

Term *term_open(bool use_alt_screen)
{
    int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    bool own = true;
    if (fd < 0) {
        /* No controlling terminal. Keep going: the caller may be piping output
         * or running in a test harness. */
        fd = STDOUT_FILENO;
        own = false;
    }

    Term *t = xcalloc(1, sizeof *t);
    t->fd = fd;
    t->own_tty = own;
    t->alt_screen = use_alt_screen;

    if (own && isatty(fd)) {
        if (tcgetattr(fd, &t->saved) == 0) {
            t->saved_ok = true;
            struct termios raw = t->saved;
            set_raw(&raw);
            tcsetattr(fd, TCSANOW, &raw);
        }
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_winch;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;                       /* no SA_RESTART: we want EINTR */
        sigaction(SIGWINCH, &sa, NULL);
    }

    if (use_alt_screen && isatty(fd)) {
        /* alt screen + hide cursor + clear */
        tty_write_all(fd, "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H", 22);
    }
    return t;
}

void term_close(Term *t)
{
    if (!t) return;
    if (t->alt_screen && isatty(t->fd)) {
        /* Show cursor, reset SGR, leave alt screen. Never leave the user's
         * terminal broken, even after an error path. */
        tty_write_all(t->fd,
            "\x1b[0m"
            "\x1b_Ga=d,d=A\x1b\\"   /* delete all kitty images, harmless elsewhere */
            "\x1b[?25h"
            "\x1b[?1049l", 0);
    }
    if (t->saved_ok) tcsetattr(t->fd, TCSANOW, &t->saved);
    if (t->own_tty && t->fd >= 0) close(t->fd);
    free(t);
}

int term_fd(const Term *t) { return t->fd; }
const TermInfo *term_info(const Term *t) { return &t->info; }

long tty_write_all(int fd, const void *data, size_t n)
{
    if (!n) {
        /* Callers pass literal sequences without a length: measure them. */
        n = strlen((const char *)data);
    }
    const char *p = data;
    size_t left = n;
    while (left) {
        ssize_t k = write(fd, p, left);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += k;
        left -= (size_t)k;
    }
    return (long)n;
}

/* ------------------------------------------------------------------ */
/* geometry                                                           */
/* ------------------------------------------------------------------ */

static bool ioctl_size(int fd, int *cols, int *rows, int *pw, int *ph)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return false;
    if (ws.ws_col) *cols = ws.ws_col;
    if (ws.ws_row) *rows = ws.ws_row;
    if (pw) *pw = ws.ws_xpixel;
    if (ph) *ph = ws.ws_ypixel;
    return true;
}

bool term_refresh_size(Term *t)
{
    int cols = t->info.cols, rows = t->info.rows, pw = 0, ph = 0;
    if (!ioctl_size(t->fd, &cols, &rows, &pw, &ph)) return false;
    bool changed = (cols != t->info.cols || rows != t->info.rows);
    t->info.cols = cols;
    t->info.rows = rows;
    if (pw > 0 && ph > 0 && cols > 0 && rows > 0) {
        t->info.px_w = pw;
        t->info.px_h = ph;
        t->info.cell_w = pw / cols;
        t->info.cell_h = ph / rows;
    }
    return changed;
}

bool term_take_resized(Term *t)
{
    (void)t;
    if (!g_resized) return false;
    g_resized = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* escape-sequence queries with hard timeouts                         */
/* ------------------------------------------------------------------ */

/* Read from fd until `pred` matches or timeout. Collects into buf.
 * Any bytes that are not part of a reply are discarded: during startup that is
 * only stray keystrokes, and the caller's input loop has not started yet. */
typedef struct {
    char   *p;
    size_t  len, cap;
} QBuf;

static void qbuf_add(QBuf *q, const char *d, size_t n)
{
    if (q->len + n + 1 > q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 256;
        while (cap < q->len + n + 1) cap *= 2;
        char *np = realloc(q->p, cap);
        if (!np) return;
        q->p = np;
        q->cap = cap;
    }
    memcpy(q->p + q->len, d, n);
    q->len += n;
    q->p[q->len] = '\0';
}

static bool query(int fd, const char *seq, size_t seqlen, int timeout_ms,
                  bool (*done)(const char *, size_t, void *), void *ctx)
{
    if (write(fd, seq, seqlen) != (ssize_t)seqlen) return false;

    QBuf q = { 0 };
    bool ok = false;
    int64_t deadline = now_ns() + (int64_t)timeout_ms * 1000000LL;

    for (;;) {
        int64_t left_ms = (deadline - now_ns()) / 1000000LL;
        if (left_ms < 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, (int)left_ms);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        char tmp[512];
        ssize_t k = read(fd, tmp, sizeof tmp);
        if (k <= 0) { if (errno == EINTR) continue; break; }
        qbuf_add(&q, tmp, (size_t)k);
        if (done(q.p ? q.p : "", q.len, ctx)) { ok = true; break; }
    }
    free(q.p);
    return ok;
}

/* Scan for an arbitrary substring reply. */
typedef struct {
    const char *needle;
} SubCtx;

static bool done_sub(const char *s, size_t n, void *ctx)
{
    (void)n;
    return strstr(s, ((SubCtx *)ctx)->needle) != NULL;
}

/* CSI 16 t -> CSI 6 ; height ; width t   (cell size in pixels)
 * CSI 14 t -> CSI 4 ; height ; width t   (window size in pixels)
 *
 * This is the query that mpv gets wrong (it only uses ioctl), so over ssh it
 * has no pixel information at all and cannot pick a real raster. We ask. */
typedef struct {
    int  code;      /* 4 or 6 */
    int *a, *b;
    bool found;
} SizeCtx;

static bool done_size(const char *s, size_t n, void *ctx)
{
    SizeCtx *c = ctx;
    for (size_t i = 0; i + 2 < n; i++) {
        if (s[i] != 0x1b || s[i + 1] != '[') continue;
        size_t j = i + 2;
        int code = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9') code = code * 10 + (s[j++] - '0');
        if (j >= n || s[j] != ';' || code != c->code) continue;
        j++;
        int h = 0, w = 0;
        while (j < n && s[j] >= '0' && s[j] <= '9') h = h * 10 + (s[j++] - '0');
        if (j >= n || s[j] != ';') continue;
        j++;
        while (j < n && s[j] >= '0' && s[j] <= '9') w = w * 10 + (s[j++] - '0');
        if (j >= n || s[j] != 't') continue;
        if (h > 0 && w > 0) {
            *c->a = w; *c->b = h; c->found = true;
            return true;
        }
    }
    return false;
}

bool term_query_cell_pixels(int fd, int *cell_w, int *cell_h)
{
    SizeCtx c = { 6, cell_w, cell_h, false };
    if (!query(fd, "\x1b[16t", 5, 120, done_size, &c)) return false;
    /* sanity: a cell is rarely larger than 128px or smaller than 3px */
    if (*cell_w < 3 || *cell_w > 128 || *cell_h < 3 || *cell_h > 200) return false;
    return true;
}

bool term_query_window_pixels(int fd, int *px_w, int *px_h)
{
    SizeCtx c = { 4, px_w, px_h, false };
    return query(fd, "\x1b[14t", 5, 120, done_size, &c);
}

bool term_query_kitty(int fd)
{
    /* Ask for support without drawing: a=q, then the terminal replies either
     * OK or an error. Both mean "understood"; silence means "no". */
    SubCtx c = { "\x1b_G" };
    static const char *probe = "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";
    return query(fd, probe, strlen(probe), 150, done_sub, &c);
}

bool term_query_sixel(int fd)
{
    /* DA1: "CSI c". Sixel-capable terminals report attribute 4 in the reply. */
    typedef struct { bool ok; } Ctx;
    Ctx c = { false };
    bool (*done)(const char *, size_t, void *) = NULL;
    (void)done;
    /* inline matcher: look for "?<attrs>c" containing ';4;' or ";4c" */
    QBuf q = { 0 };
    static const char da1[] = "\x1b[c";
    if (write(fd, da1, 3) != 3) return false;
    int64_t deadline = now_ns() + 150 * 1000000LL;
    for (;;) {
        int64_t left = (deadline - now_ns()) / 1000000LL;
        if (left < 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, (int)left);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        char tmp[256];
        ssize_t k = read(fd, tmp, sizeof tmp);
        if (k <= 0) { if (errno == EINTR) continue; break; }
        qbuf_add(&q, tmp, (size_t)k);
        if (q.p && strstr(q.p, "c")) break;
    }
    if (q.p) {
        const char *p = q.p;
        while ((p = strstr(p, ";"))) {
            if (p[1] == '4' && (p[2] == ';' || p[2] == 'c')) { c.ok = true; break; }
            p++;
        }
    }
    free(q.p);
    return c.ok;
}

/* ------------------------------------------------------------------ */
/* probing                                                            */
/* ------------------------------------------------------------------ */

static const char *env_or_empty(const char *k)
{
    const char *v = getenv(k);
    return v ? v : "";
}

static void verdict(TermInfo *in, BackendId id, bool avail, const char *fmt, ...)
{
    BackendVerdict *v = &in->verdicts[in->n_verdicts++];
    v->id = id;
    v->available = avail;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v->reason, sizeof v->reason, fmt, ap);
    va_end(ap);
}

void term_probe(Term *t, const char *force_backend)
{
    TermInfo *in = &t->info;
    memset(in, 0, sizeof *in);
    in->n_verdicts = 0;

    snprintf(in->term, sizeof in->term, "%s", env_or_empty("TERM"));
    snprintf(in->term_program, sizeof in->term_program, "%s", env_or_empty("TERM_PROGRAM"));
    snprintf(in->colorterm, sizeof in->colorterm, "%s", env_or_empty("COLORTERM"));

    in->is_tty = isatty(t->fd);
    in->cols = 80;
    in->rows = 24;
    term_refresh_size(t);

    in->in_tmux   = env_or_empty("TMUX")[0] != '\0';
    in->in_screen = str_has(in->term, "screen");
    in->in_zellij = env_or_empty("ZELLIJ")[0] != '\0';

    bool dumb = !strcmp(in->term, "dumb") || !in->is_tty;

    /* --- colours --- */
    in->truecolor = str_has(in->colorterm, "truecolor") || str_has(in->colorterm, "24bit");
    if (!in->truecolor) {
        /* These terminals are truecolor even without COLORTERM */
        static const char *const tc[] = { "kitty", "xterm-kitty", "ghostty",
            "iterm", "wezterm", "foot", "alacritty", "contour", "rio",
            "xterm-256color", NULL };
        for (int i = 0; tc[i]; i++)
            if (str_has(in->term, tc[i]) || str_has(in->term_program, tc[i])) {
                in->truecolor = true;
                break;
            }
    }

    /* --- graphics capabilities --- */
    if (!dumb) {
        /* Kitty graphics: trust the env fast-path first (no round trip),
         * then ask. Asking is what catches Ghostty/WezTerm/Konsole. */
        if (str_has(in->term_program, "ghostty") || str_has(in->term, "ghostty") ||
            str_has(in->term_program, "WezTerm") || str_has(in->term, "wezterm") ||
            str_has(in->term, "kitty") || env_or_empty("KITTY_WINDOW_ID")[0])
            in->kitty_protocol = true;
        else
            in->kitty_protocol = term_query_kitty(t->fd);

        in->iterm2_images = str_has(in->term_program, "iTerm") || str_has(in->term_program, "WezTerm") ||
                            str_has(env_or_empty("LC_TERMINAL"), "iTerm");

        /* Sixel: query, and accept a few known-good TERM values without a round trip */
        if (str_has(in->term, "foot") || str_has(in->term, "contour") ||
            str_has(in->term, "mlterm") || str_has(in->term, "yaft") ||
            str_has(in->term, "mintty") || str_has(in->term, "xterm-256color"))
            in->sixel = term_query_sixel(t->fd);
        else
            in->sixel = term_query_sixel(t->fd);

        /* tmux/screen swallow graphics escapes unless configured. Being
         * pessimistic is correct here: a broken image is worse than blocks. */
        if (in->in_tmux || in->in_screen || in->in_zellij) {
            bool passthru = env_or_empty("TERM_PROGRAM")[0] != '\0';
            if (!passthru) {
                in->kitty_protocol = false;
                in->iterm2_images = false;
                in->sixel = false;
            }
        }
    }

    /* --- pixel geometry --- */
    if (!dumb && in->is_tty) {
        if (!in->cell_w || !in->cell_h)
            term_query_cell_pixels(t->fd, &in->cell_w, &in->cell_h);
        int pw = 0, ph = 0;
        if (term_query_window_pixels(t->fd, &pw, &ph) && in->cols && in->rows) {
            in->px_w = pw; in->px_h = ph;
            if (!in->cell_w) in->cell_w = pw / in->cols;
            if (!in->cell_h) in->cell_h = ph / in->rows;
        }
    }

    /* --- the ladder, with a reason for every rung --- */
    bool force = force_backend && strcmp(force_backend, "auto") != 0;
    BackendId forced = BK__COUNT;
    if (force && !backend_parse(force_backend, &forced))
        warnx_("unknown backend '%s', falling back to auto", force_backend);
    if (force && forced == BK__COUNT) force = false;

    if (force) {
        verdict(in, forced, true, "forced on the command line");
        in->chosen = forced;
        /* still record what we would have chosen, for `player doctor` */
        return;
    }

    if (dumb) {
        verdict(in, BK_BLOCKS24, false, "TERM=dumb or not a tty");
        verdict(in, BK_KITTY,  false, "not a tty");
        verdict(in, BK_SIXEL, false, "not a tty");
        verdict(in, BK_ASCII, true,  "always available");
        in->chosen = BK_ASCII;
        return;
    }

    bool local = !env_or_empty("SSH_CONNECTION")[0] && !env_or_empty("SSH_TTY")[0];

    if (in->kitty_protocol && local) {
        verdict(in, BK_KITTY_SHM, true, "kitty graphics detected, local session (shm transfer)");
        in->chosen = BK_KITTY_SHM;
        verdict(in, BK_KITTY, true, "available as fallback");
        return;
    }
    verdict(in, BK_KITTY_SHM, false, in->kitty_protocol ? "remote session: shm unavailable"
                                                       : "kitty graphics not detected");
    if (in->kitty_protocol) {
        verdict(in, BK_KITTY, true,
                local ? "kitty graphics detected" : "kitty graphics detected over ssh (zlib transfer)");
        in->chosen = BK_KITTY;
        return;
    }
    verdict(in, BK_KITTY, false, "terminal did not answer the kitty graphics probe");

    if (in->iterm2_images) {
        verdict(in, BK_ITERM2, true, "iTerm2 inline-image protocol detected");
        in->chosen = BK_ITERM2;
        return;
    }
    verdict(in, BK_ITERM2, false, "not iTerm2/WezTerm");

    if (in->sixel) {
        verdict(in, BK_SIXEL, true, "terminal reports sixel support");
        in->chosen = BK_SIXEL;
        return;
    }
    verdict(in, BK_SIXEL, false, "terminal reports no sixel support (DA1 attr 4 absent)");

    if (in->truecolor) {
        verdict(in, BK_BLOCKS24, true, "truecolor reported by COLORTERM/TERM");
        in->chosen = BK_BLOCKS24;
        return;
    }
    verdict(in, BK_BLOCKS24, false, "no truecolor: COLORTERM is empty");
    verdict(in, BK_BLOCKS256, true, "256-colour terminal");
    verdict(in, BK_ASCII, true, "always available");
    in->chosen = BK_BLOCKS256;
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static bool read_byte(int fd, unsigned char *c, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    for (;;) {
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0) { if (errno == EINTR) return false; return false; }
        if (r == 0) return false;
        ssize_t k = read(fd, c, 1);
        if (k == 1) return true;
        if (k < 0 && errno == EINTR) continue;
        return false;
    }
}

bool term_read_key(Term *t, Key *k, int timeout_ms)
{
    memset(k, 0, sizeof *k);
    k->kind = KEY_NONE;

    if (g_resized) { g_resized = 0; k->kind = KEY_RESIZE; return true; }

    unsigned char c;
    if (!read_byte(t->fd, &c, timeout_ms)) return false;

    if (c == 0x1b) {
        unsigned char a;
        if (!read_byte(t->fd, &a, 10)) { k->kind = KEY_ESC; return true; }
        if (a != '[' && a != 'O') {
            k->kind = KEY_CHAR;
            k->alt = true;
            k->ch[0] = (char)a;
            return true;
        }
        /* CSI / SS3: read digits then a final byte */
        char seq[16] = { 0 };
        size_t n = 0;
        unsigned char f;
        while (n + 1 < sizeof seq && read_byte(t->fd, &f, 10)) {
            seq[n++] = (char)f;
            if (f >= 0x40 && f <= 0x7e) break;
        }
        char last = n ? seq[n - 1] : 0;
        if (last == 'A') k->kind = KEY_UP;
        else if (last == 'B') k->kind = KEY_DOWN;
        else if (last == 'C') k->kind = KEY_RIGHT;
        else if (last == 'D') k->kind = KEY_LEFT;
        else if (last == 'H') k->kind = KEY_HOME;
        else if (last == 'F') k->kind = KEY_END;
        else if (last == '~') {
            int num = atoi(seq);
            switch (num) {
            case 1: case 7:  k->kind = KEY_HOME; break;
            case 2:          k->kind = KEY_INSERT; break;
            case 3:          k->kind = KEY_DELETE; break;
            case 4: case 8:  k->kind = KEY_END; break;
            case 5:          k->kind = KEY_PGUP; break;
            case 6:          k->kind = KEY_PGDN; break;
            case 11: k->kind = KEY_F1; break;  case 12: k->kind = KEY_F2; break;
            case 13: k->kind = KEY_F3; break;  case 14: k->kind = KEY_F4; break;
            case 15: k->kind = KEY_F5; break;  case 17: k->kind = KEY_F6; break;
            case 18: k->kind = KEY_F7; break;  case 19: k->kind = KEY_F8; break;
            case 20: k->kind = KEY_F9; break;  case 21: k->kind = KEY_F10; break;
            case 23: k->kind = KEY_F11; break; case 24: k->kind = KEY_F12; break;
            default: k->kind = KEY_NONE; break;
            }
        } else if (last == 'P') k->kind = KEY_F1;
        else if (last == 'Q') k->kind = KEY_F2;
        else if (last == 'R') k->kind = KEY_F3;
        else if (last == 'S') k->kind = KEY_F4;
        if (k->kind == KEY_NONE) k->kind = KEY_ESC;
        return true;
    }

    if (c == '\r' || c == '\n') { k->kind = KEY_ENTER; return true; }
    if (c == '\t') { k->kind = KEY_TAB; return true; }
    if (c == 0x7f || c == 0x08) { k->kind = KEY_BACKSPACE; return true; }

    if (c < 0x20) {
        k->kind = KEY_CHAR;
        k->ctrl = true;
        k->ch[0] = (char)('a' + c - 1);   /* ctrl-a == 0x01 */
        return true;
    }

    /* UTF-8: gather continuation bytes without blocking on a lone byte. */
    k->kind = KEY_CHAR;
    k->ch[0] = (char)c;
    int extra = (c >= 0xf0) ? 3 : (c >= 0xe0) ? 2 : (c >= 0xc0) ? 1 : 0;
    for (int i = 0; i < extra; i++) {
        unsigned char cc;
        if (!read_byte(t->fd, &cc, 10)) break;
        k->ch[1 + i] = (char)cc;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* TtyWriter                                                          */
/* ------------------------------------------------------------------ */

void ttyw_init(TtyWriter *w, int fd)
{
    memset(w, 0, sizeof *w);
    w->fd = fd;
    buf_init(&w->b);
}

void ttyw_free(TtyWriter *w) { buf_free(&w->b); }

void ttyw_begin_sync(TtyWriter *w)
{
    if (w->sync_begin) return;
    buf_puts(&w->b, "\x1b[?2026h");     /* BSU: the terminal buffers until ESU */
    w->sync_begin = true;
}

void ttyw_end_sync(TtyWriter *w)
{
    if (!w->sync_begin) return;
    buf_puts(&w->b, "\x1b[?2026l");
    w->sync_begin = false;
}

long ttyw_flush(TtyWriter *w)
{
    ttyw_end_sync(w);
    if (!w->b.len) return 0;

    double t0 = now_s();
    long n = tty_write_all(w->fd, w->b.p, w->b.len);
    w->write_s += now_s() - t0;
    if (n > 0) {
        w->bytes_written += n;
        w->b.len = 0;
    }
    return n;
}

double ttyw_take_bytes_per_sec(TtyWriter *w)
{
    double s = w->write_s > 1e-6 ? w->write_s : 1e-6;
    double bps = (double)w->bytes_written / s;
    w->bytes_written = 0;
    w->write_s = 0;
    return bps;
}
