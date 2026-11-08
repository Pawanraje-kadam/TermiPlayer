/* util.h — allocation, buffers, time, small string helpers.
 *
 * Everything here is dependency-free and safe to call before the terminal is
 * in raw mode. Nothing in this header writes to stdout/stderr except die(),
 * which is only for fatal startup errors.
 */
#ifndef TP_UTIL_H
#define TP_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- fatal errors ---------------------------------------------------- */

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void warnx_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- allocation ------------------------------------------------------ */

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ---- growable byte buffer -------------------------------------------- */

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} Buf;

void   buf_init(Buf *b);
void   buf_free(Buf *b);
void   buf_reset(Buf *b);
void   buf_reserve(Buf *b, size_t extra);
void   buf_append(Buf *b, const void *data, size_t n);
void   buf_putc(Buf *b, char c);
void   buf_puts(Buf *b, const char *s);
void   buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---- time ------------------------------------------------------------ */

int64_t now_ns(void);           /* CLOCK_MONOTONIC, nanoseconds */
double  now_s(void);            /* CLOCK_MONOTONIC, seconds  */
void    sleep_ns(int64_t ns);
/* sleep until an absolute monotonic deadline; returns false on interruption */
bool    sleep_until_ns(int64_t deadline);

/* ---- little string helpers ------------------------------------------ */

bool str_has(const char *hay, const char *needle);
/* case-insensitive prefix test */
bool str_iprefix(const char *s, const char *prefix);
/* Parse "HH:MM:SS.mmm" or "MM:SS" or plain seconds. Returns -1 on failure. */
double parse_timecode(const char *s);
/* Format seconds as M:SS or H:MM:SS into buf (>= 16 bytes). */
const char *fmt_time(char *buf, size_t n, double seconds);

/* Clamp helpers */
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

#endif /* TP_UTIL_H */
