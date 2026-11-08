#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* fatal errors                                                       */
/* ------------------------------------------------------------------ */

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("player: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void warnx_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("player: warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* allocation                                                         */
/* ------------------------------------------------------------------ */

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory (%zu x %zu bytes)", n, sz);
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory (%zu bytes)", n);
    return q;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------ */
/* Buf                                                                */
/* ------------------------------------------------------------------ */

void buf_init(Buf *b) { b->p = NULL; b->len = 0; b->cap = 0; }

void buf_free(Buf *b) { free(b->p); buf_init(b); }

void buf_reset(Buf *b) { b->len = 0; }

void buf_reserve(Buf *b, size_t extra)
{
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap += cap / 2;      /* 1.5x growth, less wastage than 2x */
    b->p = xrealloc(b->p, cap);
    b->cap = cap;
}

void buf_append(Buf *b, const void *data, size_t n)
{
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void buf_putc(Buf *b, char c)
{
    buf_reserve(b, 1);
    b->p[b->len++] = c;
}

void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        /* reserve n+1: vsnprintf writes n characters *and* a NUL terminator */
        buf_reserve(b, (size_t)n + 1);
        vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
}

/* ------------------------------------------------------------------ */
/* time                                                               */
/* ------------------------------------------------------------------ */

int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

double now_s(void) { return (double)now_ns() / 1e9; }

void sleep_ns(int64_t ns)
{
    if (ns <= 0) return;
    struct timespec ts = { (time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL) };
    /* nanosleep may be interrupted; retry the remainder */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

bool sleep_until_ns(int64_t deadline)
{
    for (;;) {
        int64_t d = deadline - now_ns();
        if (d <= 0) return true;
        struct timespec ts = { (time_t)(d / 1000000000LL), (long)(d % 1000000000LL) };
        if (nanosleep(&ts, NULL) == 0) return true;
        if (errno != EINTR) return false;
    }
}

/* ------------------------------------------------------------------ */
/* strings                                                            */
/* ------------------------------------------------------------------ */

bool str_has(const char *hay, const char *needle)
{
    return hay && needle && strstr(hay, needle) != NULL;
}

bool str_iprefix(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    while (*prefix) {
        unsigned char a = (unsigned char)*s++, b = (unsigned char)*prefix++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

double parse_timecode(const char *s)
{
    if (!s || !*s) return -1;
    double total = 0;
    int fields = 0;
    const char *p = s;
    while (*p && fields < 3) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p || v < 0) return -1;
        total = total * 60.0 + v;
        fields++;
        p = end;
        if (*p == ':') p++;
        else break;
    }
    if (*p != '\0') return -1;
    return total;
}

const char *fmt_time(char *buf, size_t n, double seconds)
{
    if (seconds < 0) seconds = 0;
    long total = (long)(seconds + 0.5);
    long h = total / 3600, m = (total / 60) % 60, s = total % 60;
    if (h > 0) snprintf(buf, n, "%ld:%02ld:%02ld", h, m, s);
    else       snprintf(buf, n, "%ld:%02ld", m, s);
    return buf;
}
