/* test.h — a 40-line test harness. No framework dependency, because a project
 * that advertises "no build dependencies" should not need one to test itself. */
#ifndef TP_TEST_H
#define TP_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails, g_suite_fails;
static const char *g_suite = "?";

#define SUITE(name) do { g_suite = (name); g_suite_fails = 0; \
                         printf("\n%s\n", (name)); } while (0)
#define CHECK(cond) do { \
        g_checks++; \
        if (!(cond)) { \
            g_fails++; g_suite_fails++; \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

/* CHECK_MSG with optional extra printf args (works with zero args too) */
#define CHECK_MSG(cond, ...) do { \
        g_checks++; \
        if (!(cond)) { \
            g_fails++; g_suite_fails++; \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

#define CHECK_EQ_INT(a, b) do { \
        g_checks++; long _a = (long)(a), _b = (long)(b); \
        if (_a != _b) { \
            g_fails++; g_suite_fails++; \
            printf("  FAIL %s:%d  %s == %s (%ld != %ld)\n", \
                   __FILE__, __LINE__, #a, #b, _a, _b); \
        } \
    } while (0)

#define CHECK_NEAR(a, b, eps) do { \
        g_checks++; double _a = (double)(a), _b = (double)(b); \
        double _d = _a - _b; if (_d < 0) _d = -_d; \
        if (_d > (eps)) { \
            g_fails++; g_suite_fails++; \
            printf("  FAIL %s:%d  %s ~= %s (%.6f vs %.6f, tol %g)\n", \
                   __FILE__, __LINE__, #a, #b, _a, _b, (double)(eps)); \
        } \
    } while (0)

#define CHECK_STR(a, b) do { \
        g_checks++; const char *_a = (a), *_b = (b); \
        if (!_a || !_b || strcmp(_a, _b)) { \
            g_fails++; g_suite_fails++; \
            printf("  FAIL %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, \
                   _a ? _a : "(null)", _b ? _b : "(null)"); \
        } \
    } while (0)

static int test_report(void)
{
    printf("\n%d checks, %d failed\n", g_checks, g_fails);
    if (g_fails) printf("FAILED\n");
    else         printf("OK\n");
    return g_fails ? 1 : 0;
}

/* Helper: does `hay` contain `needle` as raw bytes? */
static int __attribute__((unused)) contains_bytes(const char *hay, size_t haylen, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0 || haylen < nl) return 0;
    for (size_t i = 0; i + nl <= haylen; i++)
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

static int __attribute__((unused)) count_bytes(const char *hay, size_t haylen, const char *needle)
{
    size_t nl = strlen(needle);
    int n = 0;
    if (nl == 0 || haylen < nl) return 0;
    for (size_t i = 0; i + nl <= haylen; i++)
        if (memcmp(hay + i, needle, nl) == 0) n++;
    return n;
}

#endif /* TP_TEST_H */
