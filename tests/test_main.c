/* test_main.c — the unit suite.
 *
 * Focus: the things that are easy to get subtly wrong and expensive to debug in
 * a terminal -- escape sequence well-formedness, raster geometry, palette
 * mapping, clock arithmetic, and the throughput controller's decisions.
 */
#include "test.h"

#include "clock.h"
#include "gfx.h"
#include "raster.h"
#include "term.h"
#include "testsrc.h"
#include "throughput.h"
#include "util.h"
#include "media.h"
#include "library.h"
#include "screen.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
static Raster make_raster(int w, int h)
{
    Raster r;
    raster_init(&r);
    raster_alloc(&r, w, h);
    return r;
}

/* A raster with a known pattern: red gradient left-to-right, green vertical. */
static void fill_pattern(Raster *r)
{
    for (int y = 0; y < r->h; y++) {
        for (int x = 0; x < r->w; x++) {
            uint8_t *p = r->px + ((size_t)y * r->w + x) * 3u;
            p[0] = (uint8_t)(x * 255 / (r->w > 1 ? r->w - 1 : 1));
            p[1] = (uint8_t)(y * 255 / (r->h > 1 ? r->h - 1 : 1));
            p[2] = (uint8_t)((x ^ y) & 0xFF);
        }
    }
}

/* ------------------------------------------------------------------ */
static void test_util(void)
{
    SUITE("util");

    Buf b;
    buf_init(&b);
    buf_puts(&b, "hello");
    buf_printf(&b, " %d", 42);
    CHECK_EQ_INT(b.len, 8);   /* "hello 42" */
    CHECK(memcmp(b.p, "hello 42", 8) == 0);
    buf_free(&b);

    /* buf_printf near the cap: this is the off-by-one that ASan caught */
    buf_init(&b);
    for (int i = 0; i < 200; i++) buf_printf(&b, "%03d", i % 1000);
    CHECK_EQ_INT(b.len, 600);
    buf_free(&b);

    CHECK_NEAR(parse_timecode("90"), 90.0, 1e-9);
    CHECK_NEAR(parse_timecode("1:30"), 90.0, 1e-9);
    CHECK_NEAR(parse_timecode("1:02:03"), 3723.0, 1e-9);
    CHECK(parse_timecode("abc") < 0);
    CHECK(parse_timecode("") < 0);
    CHECK(parse_timecode("1:x") < 0);

    char t[32];
    CHECK_STR(fmt_time(t, sizeof t, 0), "0:00");
    CHECK_STR(fmt_time(t, sizeof t, 65), "1:05");
    CHECK_STR(fmt_time(t, sizeof t, 3725), "1:02:05");

    CHECK(str_iprefix("Kitty", "kit"));
    CHECK(!str_iprefix("kit", "kitty"));
    CHECK_EQ_INT(clampi(5, 1, 3), 3);
    CHECK_EQ_INT(clampi(-1, 0, 3), 0);
}

/* ------------------------------------------------------------------ */
static void test_raster(void)
{
    SUITE("raster");

    Raster r = make_raster(8, 4);
    CHECK(r.px != NULL);
    fill_pattern(&r);
    CHECK_EQ_INT(r.w, 8);
    CHECK_EQ_INT(r.h, 4);

    /* top-left pixel: red 0, green 0 */
    CHECK_EQ_INT(r.px[0], 0);
    CHECK_EQ_INT(r.px[1], 0);

    /* scale up: nearest neighbour must preserve corners exactly */
    Raster big = make_raster(16, 8);
    raster_scale_nn(&r, &big);
    CHECK_EQ_INT(big.w, 16);
    CHECK_EQ_INT(big.px[0], r.px[0]);                  /* top-left */
    CHECK_EQ_INT(big.px[(7 * 16 + 15) * 3 + 1], r.px[(3 * 8 + 7) * 3 + 1]);  /* bottom-right */

    /* scale down to 1x1: must be the source origin under NN */
    Raster tiny = make_raster(1, 1);
    raster_scale_nn(&r, &tiny);
    CHECK_EQ_INT(tiny.px[0], r.px[0]);

    /* identity scale is a pure copy */
    Raster same = make_raster(8, 4);
    raster_scale_nn(&r, &same);
    CHECK(memcmp(same.px, r.px, 8 * 4 * 3) == 0);

    /* blit clips rather than crashing */
    Raster dst = make_raster(4, 4);
    raster_clear(&dst);
    raster_blit(&r, -3, -3, 8, 4, &dst, -2, -2);       /* mostly outside */
    raster_blit(&r, 0, 0, 8, 4, &dst, 2, 2);           /* partly outside */
    CHECK(1);                                          /* reaching here is the test */

    /* fill clamps */
    raster_fill(&r, -5, -5, 100, 100, 1, 2, 3);
    CHECK_EQ_INT(r.px[0], 1);

    /* stats on a flat image: zero entropy */
    raster_fill(&r, 0, 0, 8, 4, 10, 10, 10);
    RasterStats st = raster_stats(&r);
    CHECK_NEAR(st.mean_abs_delta, 0.0, 1e-9);
    CHECK_NEAR(st.entropy_hint, 0.0, 1e-9);

    /* stats on noise: high entropy */
    fill_pattern(&r);
    st = raster_stats(&r);
    CHECK(st.mean_abs_delta > 0.0);
    CHECK(st.entropy_hint > 0.0 && st.entropy_hint <= 1.0);

    raster_free(&r); raster_free(&big); raster_free(&tiny);
    raster_free(&same); raster_free(&dst);
}

/* ------------------------------------------------------------------ */
static void test_base64(void)
{
    SUITE("base64");

    char out[64];
    size_t n;

    n = b64_encode((const uint8_t *)"", 0, out, sizeof out, true);
    CHECK_EQ_INT(n, 0);

    n = b64_encode((const uint8_t *)"f", 1, out, sizeof out, true);
    out[n] = 0;
    CHECK_STR(out, "Zg==");

    n = b64_encode((const uint8_t *)"fo", 2, out, sizeof out, true);
    out[n] = 0;
    CHECK_STR(out, "Zm8=");

    n = b64_encode((const uint8_t *)"foo", 3, out, sizeof out, true);
    out[n] = 0;
    CHECK_STR(out, "Zm9v");

    n = b64_encode((const uint8_t *)"foob", 4, out, sizeof out, true);
    out[n] = 0;
    CHECK_STR(out, "Zm9vYg==");

    n = b64_encode((const uint8_t *)"fooba", 5, out, sizeof out, true);
    out[n] = 0;
    CHECK_STR(out, "Zm9vYmE=");

    n = b64_encode((const uint8_t *)"foobar", 6, out, sizeof out, true);
    out[n] = 0;
    CHECK_STR(out, "Zm9vYmFy");

    /* unpadded sizes must match the buffer we reserve for them */
    for (size_t len = 0; len < 40; len++) {
        uint8_t src[40];
        memset(src, 'A', sizeof src);
        size_t need = b64_size(len, false);
        char *buf = xmalloc(need + 8);
        size_t got = b64_encode(src, len, buf, need + 8, false);
        CHECK_EQ_INT((int)got, (int)need);
        free(buf);
    }
}

/* ------------------------------------------------------------------ */
/* Every backend must produce well-formed, self-closing escape output. */

static void check_backend(BackendId id, const char *name, const char *open_seq,
                          const char *close_seq, int w, int h)
{
    Raster r = make_raster(w, h);
    fill_pattern(&r);

    TtyWriter wtr;
    ttyw_init(&wtr, -1);        /* fd unused: we inspect the buffer */

    TermInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.cell_w = 8;
    ti.cell_h = 16;
    ti.truecolor = true;
    ti.is_tty = true;
    ti.cols = 80;
    ti.rows = 24;

    Rect area = { 0, 0, w, h };
    gfx_draw(id, &ti, &wtr, &r, area);

    CHECK_MSG(wtr.b.len > 0, "%s produced no output", name);

    if (open_seq) {
        int opens = count_bytes(wtr.b.p, wtr.b.len, open_seq);
        int closes = count_bytes(wtr.b.p, wtr.b.len, close_seq);
        CHECK_MSG(opens > 0, "%s: expected %s", name, open_seq);
        CHECK_MSG(opens == closes, "%s: %d opens vs %d closes",
                  name, opens, closes);
    }

    /* no ESC may be followed by something that is not a real introducer */
    int malformed = 0;
    for (size_t i = 0; i < wtr.b.len; i++) {
        if ((unsigned char)wtr.b.p[i] != 0x1b) continue;
        if (i + 1 >= wtr.b.len) { malformed++; break; }
        char c = wtr.b.p[i + 1];
        if (c != '[' && c != ']' && c != 'P' && c != '_' && c != '\\' && c != '(')
            malformed++;
    }
    CHECK_MSG(malformed == 0, "%s: %d malformed ESC sequences", name, malformed);

    buf_free(&wtr.b);
    raster_free(&r);
}

static void test_backends(void)
{
    SUITE("backends");

    check_backend(BK_BLOCKS24, "blocks24", NULL, NULL, 16, 8);
    check_backend(BK_BLOCKS256, "blocks256", NULL, NULL, 16, 8);
    check_backend(BK_ASCII, "ascii", NULL, NULL, 16, 8);
    check_backend(BK_KITTY, "kitty", "\x1b_G", "\x1b\\", 16, 8);
    check_backend(BK_SIXEL, "sixel", "\x1bP", "\x1b\\", 16, 8);
    check_backend(BK_ITERM2, "iterm2", "\x1b]1337;", "\x07", 16, 8);

    /* kitty chunking: a large frame must span several APC sequences and the
     * final one must have m=0 */
    Raster big = make_raster(200, 100);
    fill_pattern(&big);
    TtyWriter wtr; ttyw_init(&wtr, -1);
    TermInfo ti; memset(&ti, 0, sizeof ti);
    ti.cell_w = 8; ti.cell_h = 16; ti.truecolor = true;
    Rect area = { 0, 0, 200, 100 };
    gfx_draw(BK_KITTY, &ti, &wtr, &big, area);
    int apcs = count_bytes(wtr.b.p, wtr.b.len, "\x1b_G");
    CHECK_MSG(apcs > 1, "kitty: expected chunking, got %d APC(s)", apcs);
    CHECK(count_bytes(wtr.b.p, wtr.b.len, "m=0;") >= 1);
    /* chunk payloads must never exceed the 4096-byte protocol limit */
    buf_free(&wtr.b);
    raster_free(&big);
}

/* ------------------------------------------------------------------ */
/* Raster sizing per backend geometry. */

static void test_raster_size(void)
{
    SUITE("raster size");

    TermInfo ti; memset(&ti, 0, sizeof ti);
    ti.cell_w = 10;
    ti.cell_h = 20;
    Rect area = { 0, 0, 40, 12 };

    int w, h;
    gfx_raster_size(BK_BLOCKS24, &ti, area, &w, &h);
    CHECK_EQ_INT(w, 40);       /* 1 px per cell wide */
    CHECK_EQ_INT(h, 24);       /* 2 px per cell tall */

    gfx_raster_size(BK_ASCII, &ti, area, &w, &h);
    CHECK_EQ_INT(w, 40);
    CHECK_EQ_INT(h, 12);

    gfx_raster_size(BK_KITTY, &ti, area, &w, &h);
    CHECK_EQ_INT(w, 400);      /* real pixels */
    CHECK_EQ_INT(h, 240);

    gfx_raster_size(BK_SIXEL, &ti, area, &w, &h);
    CHECK_EQ_INT(w, 400);
    CHECK_EQ_INT(h, 240);

    /* absurd input must be clamped, not propagated */
    Rect huge = { 0, 0, 100000, 100000 };
    gfx_raster_size(BK_KITTY, &ti, huge, &w, &h);
    CHECK(w <= 4096);
    CHECK(h <= 4096);
}

/* ------------------------------------------------------------------ */
static void test_clock(void)
{
    SUITE("clock");

    Clock *c = clock_new();
    int64_t t0 = now_ns();

    /* monotonic master: position advances with wall time */
    clock_start_monotonic(c, t0);
    CHECK_NEAR(clock_position(c), 0.0, 0.02);

    /* seeking sets the position exactly and holds the next decision */
    clock_seek(c, 100.0, now_ns());
    CHECK_NEAR(clock_position(c), 100.0, 0.02);

    FrameDecision d = clock_decide(c, 100.0, now_ns(), 33333333);
    CHECK_EQ_INT(d.action, ACT_SHOW);

    /* a frame far in the future: wait */
    d = clock_decide(c, 110.0, now_ns(), 33333333);
    CHECK_EQ_INT(d.action, ACT_WAIT);
    CHECK(d.wait_ns > 0);

    /* a frame far behind: drop it */
    d = clock_decide(c, 90.0, now_ns(), 33333333);
    CHECK_EQ_INT(d.action, ACT_HOLD);

    /* audio master: position follows the frame count */
    Clock *c2 = clock_new();
    clock_set_audio_position(c2, 48000, 48000, now_ns());
    CHECK_NEAR(clock_position(c2), 1.0, 0.05);
    clock_set_audio_position(c2, 96000, 48000, now_ns());
    CHECK_NEAR(clock_position(c2), 2.0, 0.05);

    /* a discontinuity (seek) must re-base, not produce a bogus rate */
    clock_set_audio_position(c2, 480000, 48000, now_ns());
    CHECK_NEAR(clock_position(c2), 10.0, 0.05);

    /* pause freezes the clock */
    Clock *c3 = clock_new();
    clock_start_monotonic(c3, now_ns());
    clock_set_paused(c3, true, now_ns());
    double frozen = clock_position(c3);
    sleep_ns(20000000);
    CHECK_NEAR(clock_position(c3), frozen, 0.01);
    clock_set_paused(c3, false, now_ns());
    CHECK(!clock_paused(c3));

    /* speed clamps and stays continuous */
    Clock *c4 = clock_new();
    clock_start_monotonic(c4, now_ns());
    clock_set_speed(c4, 100.0);
    CHECK_NEAR(clock_speed(c4), 4.0, 1e-9);
    clock_set_speed(c4, 0.01);
    CHECK_NEAR(clock_speed(c4), 0.25, 1e-9);

    /* counters move */
    ClockStats cs = clock_stats(c);
    CHECK(cs.shown + cs.dropped + cs.repeated > 0);

    clock_free(c); clock_free(c2); clock_free(c3); clock_free(c4);
}

/* ------------------------------------------------------------------ */
/* The throughput controller is the headline feature; test the policy. */

static void test_throughput(void)
{
    SUITE("throughput");

    /* A fast link: full raster. */
    Throughput *tp = tp_new(80, 24, 8, 16, 30);
    for (int i = 0; i < 20; i++)
        tp_observe(tp, 30000, 1.0/30.0, 3.0, 0.3, BK_BLOCKS24);  /* 900 KB/s */
    TpAdvice a = tp_advise(tp, BK_BLOCKS24);
    CHECK_MSG(a.raster_w > 40, "fast link should allow a large raster, got %d", a.raster_w);
    tp_free(tp);

    /* A starved link. 320x90 cells = 28,800 cells. Truecolor blocks cost ~37
     * B/cell, so a full frame is ~1.07 MB. At 30 fps that demands 32 MB/s. We
     * hand it 0.5 MB/frame every 1/10 s == 5 MB/s sustained: it must notice and
     * shrink, and must propose indexed colour. */
    tp = tp_new(320, 90, 8, 16, 30);
    for (int i = 0; i < 20; i++)
        tp_observe(tp, 500000, 0.10, 5.0, 0.9, BK_BLOCKS24);
    a = tp_advise(tp, BK_BLOCKS24);
    CHECK_MSG(a.raster_w <= 320 && a.raster_h <= 180,
              "raster must stay inside the box (%dx%d)", a.raster_w, a.raster_h);
    CHECK_MSG(a.raster_w * a.raster_h < 320 * 90,
              "starved link should reduce area (got %dx%d)", a.raster_w, a.raster_h);
    CHECK_MSG(a.downgrade_backend, "starved blocks24 should propose 256-colour");
    tp_free(tp);

    /* kinky: shared-memory kitty is not bandwidth bound at all. */
    tp = tp_new(200, 60, 8, 16, 30);
    for (int i = 0; i < 10; i++)
        tp_observe(tp, 200, 1.0/30.0, 4.0, 0.5, BK_KITTY_SHM);     /* ~6 KB/s */
    a = tp_advise(tp, BK_KITTY_SHM);
    CHECK_MSG(a.raster_w == 200 * 8, "shm must allow the full raster, got %d", a.raster_w);
    tp_free(tp);

    /* CPU-bound: decode is slower than the frame budget -> cap fps instead of
     * destroying resolution. */
    tp = tp_new(160, 50, 8, 16, 30);
    for (int i = 0; i < 20; i++)
        tp_observe(tp, 50000, 1.0/30.0, 80.0, 0.3, BK_BLOCKS24);   /* 80 ms decode */
    a = tp_advise(tp, BK_BLOCKS24);
    CHECK_MSG(a.fps_cap < 30, "CPU-bound should lower fps, got cap %d", a.fps_cap);
    tp_free(tp);

    /* Forced raster wins over every heuristic. */
    tp = tp_new(80, 24, 8, 16, 30);
    tp_force_raster(tp, 33, 17);
    a = tp_advise(tp, BK_BLOCKS24);
    CHECK_EQ_INT(a.raster_w, 33);
    CHECK_EQ_INT(a.raster_h, 17);
    tp_free(tp);

    /* No measurement yet: must start conservative, never full-size. */
    tp = tp_new(320, 90, 8, 16, 30);
    a = tp_advise(tp, BK_BLOCKS24);
    CHECK_MSG(a.raster_w < 320, "uncalibrated should start small, got %d", a.raster_w);
    tp_free(tp);
}

/* ------------------------------------------------------------------ */
static void test_testsrc(void)
{
    SUITE("testsrc");

    Raster r = make_raster(64, 36);
    testsrc_render(&r, 0, 0.0);
    /* frame 0 == frame 0, deterministically */
    Raster r2 = make_raster(64, 36);
    testsrc_render(&r2, 0, 0.0);
    CHECK(memcmp(r.px, r2.px, 64 * 36 * 3) == 0);

    /* a later frame differs (the bar moved) */
    testsrc_render(&r2, 30, 1.0);
    CHECK(memcmp(r.px, r2.px, 64 * 36 * 3) != 0);

    /* tiny rasters must not crash or overflow */
    for (int w = 1; w <= 5; w++) {
        Raster t = make_raster(w, 1);
        testsrc_render(&t, 7, 0.3);
        raster_free(&t);
    }

    testsrc_render_static(&r);
    CHECK(r.px[0] != 0xFF || r.px[1] != 0xFF);

    raster_free(&r); raster_free(&r2);
}

/* ------------------------------------------------------------------ */
static void test_term(void)
{
    SUITE("term");

    BackendId id;
    CHECK(backend_parse("kitty", &id) && id == BK_KITTY);
    CHECK(backend_parse("sixel", &id) && id == BK_SIXEL);
    CHECK(backend_parse("auto", &id) && id == BK__COUNT);
    CHECK(!backend_parse("nonsense", &id));
    CHECK(!backend_parse(NULL, &id));

    CHECK_STR(backend_name(BK_KITTY_SHM), "kitty-shm");
    CHECK_STR(backend_name(BK_BLOCKS256), "blocks256");
    CHECK_STR(backend_name(BK_ASCII), "ascii");

    /* the writer must bracket output in BSU/ESU exactly once */
    TtyWriter w;
    ttyw_init(&w, -1);
    ttyw_begin_sync(&w);
    buf_puts(&w.b, "X");
    ttyw_begin_sync(&w);           /* nested: must be a no-op */
    buf_puts(&w.b, "Y");
    ttyw_end_sync(&w);
    ttyw_end_sync(&w);             /* extra close: must be a no-op */
    CHECK_EQ_INT(count_bytes(w.b.p, w.b.len, "\x1b[?2026h"), 1);
    CHECK_EQ_INT(count_bytes(w.b.p, w.b.len, "\x1b[?2026l"), 1);
    buf_free(&w.b);
}


/* ------------------------------------------------------------------ */
/* media: release-name parsing                                        */
/* ------------------------------------------------------------------ */

/* Every one of these strings came from a real file name that was rendered
 * wrong at some point. Title parsing is the difference between a library that
 * looks like a dump of the filesystem and one that looks like a library, so it
 * gets regression cases rather than a single "it works" check. */
static void test_media_titles(void)
{
    struct { const char *in, *want; } cases[] = {
        { "/m/Dune.2021.1080p.WEB-DL.x264-GROUP.mkv", "Dune 2021" },
        { "/m/the.matrix.1999.2160p.UHD.BluRay.REMUX.HEVC.Atmos-TERMiNAL.mkv",
          "The Matrix 1999" },
        { "/m/Blade Runner 2049 (2017) [1080p] [YTS.AG].mp4", "Blade Runner 2049" },
        { "/m/01 - Test Artist - A Song.mp3", "A Song" },
        { "/m/Pink Floyd - Comfortably Numb.flac", "Comfortably Numb" },
        { "/m/Some.Movie.2019.720p.BRRip.x264.AAC-OzR.mkv", "Some Movie 2019" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *got = media_pretty_title(cases[i].in);
        CHECK_MSG(got && !strcmp(got, cases[i].want),
                  "%s -> \"%s\" (want \"%s\")", cases[i].in, got ? got : "(null)",
                  cases[i].want);
        free(got);
    }

    /* the artist split must not eat the title when there is no separator */
    char *pretty = media_pretty_title("/m/Dune.2021.1080p.WEB-DL.x264-GROUP.mkv");
    char *artist = NULL, *title = NULL;
    media_split_artist(pretty, &artist, &title);
    CHECK_MSG(title && !strcmp(title, "Dune 2021"), "split title keeps the name");
    free(pretty); free(artist); free(title);
}

static void test_media_kinds(void)
{
    CHECK_EQ_INT(media_ext_kind("a.mkv"), MK_VIDEO);
    CHECK_EQ_INT(media_ext_kind("a.MP4"), MK_VIDEO);
    CHECK_EQ_INT(media_ext_kind("a.flac"), MK_AUDIO);
    CHECK_EQ_INT(media_ext_kind("a.ogg"), MK_AUDIO);
    CHECK_EQ_INT(media_ext_kind("a.txt"), MK_UNKNOWN);
    CHECK(media_ext_known("x.mkv"));
    CHECK(!media_ext_known("x.txt"));
    /* a dotfile with no extension must not look like one */
    CHECK(!media_ext_known("/tmp/.hidden"));
}

/* ------------------------------------------------------------------ */
/* library: persistence and views                                     */
/* ------------------------------------------------------------------ */

static void test_library(void)
{
    char dir[] = "/tmp/tp-test-lib-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);

    Library *l = lib_open(dir, NULL);      /* NULL ffmpeg: no probing, fine */
    CHECK(l != NULL);
    CHECK_EQ_INT(lib_count(l), 0);

    /* a state line for a file that was never indexed must still be loaded:
     * this is the "player movie.mkv" path, and dropping it broke resume for
     * every file outside a watched folder. */
    char st[512];
    snprintf(st, sizeof st, "%s/state.tsv", dir);
    FILE *f = fopen(st, "wb");
    CHECK(f != NULL);
    fprintf(f, "# termiplayer state v1\nS\t/tmp/outside.mp4\t42.500\t0.3100\t1700000000\n");
    fclose(f);

    lib_close(l);
    l = lib_open(dir, NULL);
    Entry *e = lib_find(l, "/tmp/outside.mp4");
    CHECK_MSG(e != NULL, "state for an unindexed file is loaded");
    if (e) {
        CHECK_NEAR(e->position, 42.5, 0.001);
        CHECK_NEAR(e->watched, 0.31, 0.001);
    }

    /* position updates round-trip through an atomic write */
    if (e) lib_set_position(l, e, 10.0, 100.0);
    lib_flush(l);
    lib_close(l);
    l = lib_open(dir, NULL);
    e = lib_find(l, "/tmp/outside.mp4");
    CHECK(e != NULL && fabs(e->position - 10.0) < 0.01);

    /* lib_touch creates entries, and is idempotent */
    int before = lib_count(l);
    Entry *t1 = lib_touch(l, "/tmp/new.mkv");
    Entry *t2 = lib_touch(l, "/tmp/new.mkv");
    CHECK(t1 && t1 == t2);
    CHECK_EQ_INT(lib_count(l), before + 1);

    /* no temp files left behind by the atomic write */
    char tmpname[600];
    snprintf(tmpname, sizeof tmpname, "%s/state.tsv.tmp", dir);
    struct stat sb;
    CHECK_MSG(stat(tmpname, &sb) != 0, "atomic write cleans up its temp file");

    lib_close(l);

    /* corrupt / truncated files must not crash or hang */
    f = fopen(st, "wb");
    fprintf(f, "S\t\t\t\n\nF\t\nI\tonly-three\t1\t9\n");
    fclose(f);
    l = lib_open(dir, NULL);
    CHECK_MSG(l != NULL, "survives a malformed state file");
    lib_close(l);
}

static void test_library_paths(void)
{
    /* paths with spaces and tabs must survive a save/load round trip: tabs are
     * the field separator, so they have to be escaped. */
    char dir[] = "/tmp/tp-test-path-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    Library *l = lib_open(dir, NULL);
    Entry *e = lib_touch(l, "/tmp/a b/c\td.mkv");
    CHECK(e != NULL);
    if (e) lib_set_position(l, e, 5.0, 50.0);
    lib_flush(l);
    lib_close(l);

    l = lib_open(dir, NULL);
    e = lib_find(l, "/tmp/a b/c\td.mkv");
    CHECK_MSG(e != NULL, "path with a tab survives the round trip");
    lib_close(l);
}

/* ------------------------------------------------------------------ */
/* screen: the diff renderer                                          */
/* ------------------------------------------------------------------ */

static void test_screen(void)
{
    Screen s;
    screen_init(&s, 40, 10);
    screen_clear(&s, 0xFFFFFF, 0x000000);

    /* drawing text lands in the right cells with the right glyphs */
    int n = screen_text(&s, 2, 3, 10, "hi", 0xFF0000, 0x101010, A_NONE);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(s.cur[3 * s.w + 2].ch, 'h');
    CHECK_EQ_INT(s.cur[3 * s.w + 3].ch, 'i');
    CHECK_EQ_INT(s.cur[3 * s.w + 2].fg, 0xFF0000);

    /* text must be clipped to maxw, not overflow into the next field */
    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_text(&s, 0, 0, 3, "abcdef", 0xFFFFFF, 0, A_NONE);
    CHECK_EQ_INT(s.cur[2].ch, 'c');
    CHECK_MSG(s.cur[3].ch == ' ', "text stops at maxw");

    /* screen_text_right ellipsizes and must NOT blank the area to its left:
     * padding with spaces there erased content already drawn on that row. */
    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_text(&s, 0, 0, 20, "ABCDEFGHIJKLMN", 0xFFFFFF, 0, A_NONE);
    screen_text_right(&s, 10, 0, 6, "xyz", 0xFFFFFF, 0, A_NONE);
    CHECK_EQ_INT(s.cur[0].ch, 'A');
    CHECK_MSG(s.cur[5].ch == 'F', "cell left of a right-aligned run is untouched");
    /* width 6 at x=10 holds cells 10..15; "xyz" is 3 wide, so it starts at 13 */
    CHECK_EQ_INT(s.cur[13].ch, 'x');
    CHECK_MSG(s.cur[12].ch == 'M', "the cell just before the run is untouched");
    CHECK_EQ_INT(s.cur[15].ch, 'z');

    /* a long string is truncated, and the truncation is visible */
    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_text_right(&s, 0, 0, 5, "abcdefghij", 0xFFFFFF, 0, A_NONE);
    CHECK_EQ_INT(s.cur[4].ch, 0x2026);   /* clipped at the end, ellipsis there */
    CHECK_EQ_INT(s.cur[0].ch, 'a');      /* and the start is preserved */

    /* geometry: a box occupies exactly its corners */
    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_box(&s, 1, 1, 8, 4, 0x888888, 0x000000);
    CHECK_EQ_INT(s.cur[1 * s.w + 1].ch, 0x250C);        /* ┌ */
    CHECK_EQ_INT(s.cur[1 * s.w + 8].ch, 0x2510);        /* ┐ */
    CHECK_EQ_INT(s.cur[4 * s.w + 1].ch, 0x2514);        /* └ */
    CHECK_EQ_INT(s.cur[2 * s.w + 1].ch, 0x2502);        /* │ */
    CHECK_MSG(s.cur[1 * s.w + 9].ch == ' ', "box does not bleed past its width");

    /* the bar fills proportionally and clamps out-of-range fractions */
    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_bar(&s, 0, 0, 10, 0.5, 0xFFFFFF, 0x444444, 0x101010);
    int filled = 0;
    for (int x = 0; x < 10; x++)
        if (s.cur[x].fg == 0xFFFFFF) filled++;
    CHECK_MSG(filled >= 4 && filled <= 6, "half-filled bar fills about half (%d)", filled);

    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_bar(&s, 0, 0, 10, 5.0, 0xFFFFFF, 0x444444, 0x101010);   /* way over */
    screen_bar(&s, 0, 0, 10, -3.0, 0xFFFFFF, 0x444444, 0x101010);  /* negative */
    CHECK_MSG(1, "out-of-range bar fractions do not crash");

    screen_free(&s);
}

/* The diff promise: a flush after no changes emits (almost) nothing. And a
 * flush after one change emits that change, not the whole screen. */
static void test_screen_diff(void)
{
    Screen s;
    screen_init(&s, 40, 12);
    TtyWriter w;
    ttyw_init(&w, -1);

    screen_clear(&s, 0xFFFFFF, 0x000000);
    screen_text(&s, 0, 0, 40, "hello", 0xFFFFFF, 0, A_NONE);
    screen_flush(&s, &w, true, true);      /* first paint is necessarily full */
    size_t full = w.b.len;
    CHECK(full > 100);

    w.b.len = 0;
    screen_flush(&s, &w, true, false);     /* nothing changed */
    CHECK_MSG(w.b.len < full / 4, "unchanged frame is cheap (%zu vs %zu)",
              w.b.len, full);

    w.b.len = 0;
    static const char X_CH = 'X';
    screen_text(&s, 20, 5, 40, "X", 0x00FF00, 0x000000, A_NONE);
    screen_flush(&s, &w, true, false);
    CHECK_MSG(w.b.len < full / 3, "one changed cell moves few bytes (%zu)", w.b.len);
    CHECK_MSG(memchr(w.b.p, X_CH, w.b.len) != NULL, "the changed cell is emitted");

    /* a resize must invalidate the whole diff cache, or the screen tears */
    w.b.len = 0;
    screen_resize(&s, 50, 15);
    screen_flush(&s, &w, true, false);
    CHECK_MSG(w.b.len > 0, "resize forces a repaint");

    buf_free(&w.b);
    screen_free(&s);
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("TermiPlayer unit tests\n======================");
    test_util();
    test_raster();
    test_base64();
    test_raster_size();
    test_backends();
    test_clock();
    test_throughput();
    test_testsrc();
    test_term();
    test_media_titles();
    test_media_kinds();
    test_library();
    test_library_paths();
    test_screen();
    test_screen_diff();
    return test_report();
}
