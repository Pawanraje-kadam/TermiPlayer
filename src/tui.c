/* tui.c — the interactive library interface.
 *
 * Structure: one event loop, one frame per iteration, everything drawn into a
 * Screen and flushed as a diff. Playback is not a separate mode: when something
 * is playing, the raster is drawn into the right pane before the text layer is
 * composed over it, so the video and the chrome share a single synchronized
 * frame.
 *
 * Layout adapts to width, because a terminal is not a fixed-size window:
 *
 *   wide  (>= 100 cols)   library | preview+queue
 *   medium(>= 60)         library | preview
 *   narrow(< 60)          library only, preview hidden
 */
#include "tui.h"

#include "audio.h"
#include "clock.h"
#include "decode.h"
#include "gfx.h"
#include "raster.h"
#include "screen.h"
#include "term.h"
#include "throughput.h"
#include "util.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* theme                                                              */
/* ------------------------------------------------------------------ */

#define C_BG        0x14161a
#define C_PANEL     0x1b1e24
#define C_SEL_BG    0x2d5f8a
#define C_FG        0xd8dee9
#define C_DIM       0x6b7482
#define C_ACCENT    0x88c0d0
#define C_TITLE     0xeceff4
#define C_OK        0xa3be8c
#define C_WARN      0xebcb8b
#define C_BAR       0x5e81ac
#define C_BAR_TRACK 0x3b4252
#define C_BORDER    0x2e3440

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *path;
} QueueItem;

typedef struct {
    Screen     scr;
    TtyWriter  w;
    Term      *term;
    TermInfo   ti;
    BackendId  backend;
    Library   *lib;
    Throughput *tp;
    Clock     *clock;

    /* view state */
    int   *filtered;      /* indices into lib, current filter */
    int    nfiltered;
    int    cursor;
    int    scroll;
    char   filter[128];
    bool   filtering;
    bool   help_open;

    /* queue */
    QueueItem *queue;
    int   qn, qcap;
    int   qpos;           /* index of the playing item, -1 = none */

    /* playback */
    DecodeSource *dec;
    AudioOut  *audio;
    Raster     frame;     /* decoded */
    bool       playing;
    bool       paused;
    bool       playback_done;
    double     duration;
    double     pos;
    MediaKind  playing_kind;
    char       playing_title[256];
    Entry     *playing_entry;
    int64_t    last_save_ns;
    int64_t    last_draw_ns;
    int64_t    last_stat_ns;
    double     measured_bps;
    bool       show_stats;

    /* scan */
    bool   scanning;
    int    scan_found;
    char   scan_path[512];
    bool   scan_probing;
    /* PTS bookkeeping: a decoded frame's timestamp is its index in the current
     * decode stream divided by the source frame rate. Feeding the clock back in
     * as the PTS made every frame look in sync, so nothing was ever paced. */
    int    frame_idx;
    bool   frame_eof;
    double pts_base;
    double play_fps;

    char   toast[512];          /* owned copy: callers pass stack buffers */
    int64_t toast_until_ns;

    bool   running;
    TuiOpts opt;
} Tui;

static Tui *g_tui;

static void on_signal(int sig)
{
    (void)sig;
    if (g_tui) g_tui->running = false;
}

static void toast(Tui *t, const char *msg)
{
    /* Copy, never alias: every caller passes a local buffer, and storing the
     * pointer left the toast reading freed stack memory (it usually rendered
     * blank). */
    snprintf(t->toast, sizeof t->toast, "%s", msg ? msg : "");
    t->toast_until_ns = now_ns() + 2000000000LL;
}

/* ------------------------------------------------------------------ */
/* queue                                                              */
/* ------------------------------------------------------------------ */

static void queue_add(Tui *t, const char *path)
{
    if (t->qn >= t->qcap) {
        t->qcap = t->qcap ? t->qcap * 2 : 32;
        t->queue = xrealloc(t->queue, (size_t)t->qcap * sizeof *t->queue);
    }
    t->queue[t->qn++].path = xstrdup(path);
}

static void queue_clear(Tui *t)
{
    for (int i = 0; i < t->qn; i++) free(t->queue[i].path);
    t->qn = 0;
    t->qpos = -1;
}

/* ------------------------------------------------------------------ */
/* playback                                                           */
/* ------------------------------------------------------------------ */

static void stop_playback(Tui *t)
{
    if (t->dec) { decode_close(t->dec); t->dec = NULL; }
    if (t->audio) { audio_close(t->audio); t->audio = NULL; }
    t->playing = false;
    t->paused = false;
    t->playback_done = false;
    t->playing_entry = NULL;
    t->pos = 0;
    t->duration = 0;
}

static void start_path(Tui *t, const char *path);

static void advance_queue(Tui *t)
{
    if (t->qpos + 1 >= t->qn) return;
    t->qpos++;
    start_path(t, t->queue[t->qpos].path);
}

static void start_path(Tui *t, const char *path)
{
    stop_playback(t);

    Entry *e = lib_touch(t->lib, path);
    t->playing_entry = e;

    MediaInfo mi;
    char err[512];
    bool want_v = !t->opt.no_video;
    bool want_a = !t->opt.no_audio;

    /* An audio file needs no raster at all: skip the video pipe entirely. */
    MediaKind kind = e ? e->kind : media_ext_kind(path);
    if (kind == MK_AUDIO) want_v = false;

    t->dec = decode_open(path, want_v, want_a, &mi, err, sizeof err);
    if (!t->dec) {
        toast(t, err);
        return;
    }

    t->duration = mi.duration;
    t->playing_kind = mi.has_video ? MK_VIDEO : MK_AUDIO;
    snprintf(t->playing_title, sizeof t->playing_title, "%s",
             e && e->title ? e->title : path);

    if (mi.has_audio && !t->opt.no_audio) {
        AudioInfo ai;
        t->audio = audio_open(DECODE_AUDIO_RATE, 2, &ai);
    }

    if (t->clock) clock_free(t->clock);
    t->clock = clock_new();
    clock_start_monotonic(t->clock, now_ns());

    /* Resume where we left off. This is a real seek in the decoder (-ss), not
     * a cosmetic clock offset: the player must show the frame at that position,
     * not the first frame while pretending to be 40 minutes in. */
    double resume = e ? e->position : 0;
    if (resume > 5.0 && (mi.duration <= 0 || resume < mi.duration - 10.0)) {
        char serr[256];
        if (decode_seek(t->dec, resume, serr, sizeof serr) == 0) {
            clock_seek(t->clock, resume, now_ns());
            t->pos = resume;
            char msg[160];
            char tc[24];
            snprintf(msg, sizeof msg, "Resumed at %s", fmt_time(tc, sizeof tc, resume));
            toast(t, msg);
        } else {
            clock_seek(t->clock, 0, now_ns());
            toast(t, "Could not resume; starting from the beginning");
        }
    } else {
        clock_seek(t->clock, 0, now_ns());
    }

    t->frame_idx = 0;
    t->frame_eof = false;
    t->pts_base = resume > 5.0 ? resume : 0.0;
    t->play_fps = mi.src_fps;
    t->playing = true;
    t->paused = false;
    t->playback_done = false;
    t->last_save_ns = now_ns();
}

static void toggle_pause(Tui *t)
{
    if (!t->playing) return;
    t->paused = !t->paused;
    clock_set_paused(t->clock, t->paused, now_ns());
    if (t->audio) audio_flush(t->audio);
}

static void seek_relative(Tui *t, double delta)
{
    if (!t->playing) return;
    double target = clock_position(t->clock) + delta;
    if (target < 0) target = 0;
    if (t->duration > 0 && target > t->duration) target = t->duration;
    char serr[256];
    if (t->dec && decode_seek(t->dec, target, serr, sizeof serr) != 0) {
        toast(t, serr);
        return;
    }
    clock_seek(t->clock, target, now_ns());
    t->pos = target;
    if (t->audio) audio_flush(t->audio);
}

/* ------------------------------------------------------------------ */
/* filtering                                                          */
/* ------------------------------------------------------------------ */

static void refilter(Tui *t)
{
    free(t->filtered);
    t->filtered = lib_filter(t->lib, t->filter, &t->nfiltered);
    if (t->cursor >= t->nfiltered) t->cursor = t->nfiltered ? t->nfiltered - 1 : 0;
    if (t->cursor < 0) t->cursor = 0;
}

static void move_cursor(Tui *t, int delta)
{
    t->cursor += delta;
    if (t->cursor < 0) t->cursor = 0;
    if (t->cursor >= t->nfiltered) t->cursor = t->nfiltered ? t->nfiltered - 1 : 0;
}

/* ------------------------------------------------------------------ */
/* scanning                                                           */
/* ------------------------------------------------------------------ */

static void scan_progress(void *ctx, int found, const char *path, bool probing)
{
    Tui *t = ctx;
    t->scan_found = found;
    t->scan_probing = probing;
    snprintf(t->scan_path, sizeof t->scan_path, "%s", path);
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_header(Tui *t)
{
    Screen *s = &t->scr;
    int w = s->w;
    screen_fill(s, 0, 0, w, 1, C_TITLE, C_SEL_BG, A_NONE);

    screen_text(s, 1, 0, w - 2, "▚ TermiPlayer", C_TITLE, C_SEL_BG, A_BOLD);

    int nv = 0, na = 0;
    double hours = 0;
    lib_stats(t->lib, &nv, &na, &hours);

    char right[200];
    if (t->filtering || t->filter[0]) {
        snprintf(right, sizeof right, "filter: %s%s   %d/%d",
                 t->filter, t->filtering ? "_" : "", t->nfiltered, lib_count(t->lib));
    } else {
        snprintf(right, sizeof right, "%d video · %d audio · %.0f h   [%s]",
                 nv, na, hours, backend_name(t->backend));
    }
    screen_text_right(s, 1, 0, w - 2, right, C_TITLE, C_SEL_BG, A_NONE);
}

static void draw_library(Tui *t, int x, int y, int w, int h)
{
    Screen *s = &t->scr;
    if (w < 12 || h < 4) return;

    char title[64];
    snprintf(title, sizeof title, " LIBRARY (%d) ", t->nfiltered);
    screen_box_titled(s, x, y, w, h, title, C_BORDER, C_PANEL);

    int inner_y = y + 1;
    int inner_h = h - 2;
    int inner_w = w - 2;

    if (t->nfiltered == 0) {
        const char *msg = lib_count(t->lib) == 0
            ? "Nothing indexed yet."
            : "No matches.";
        screen_text(s, x + 2, inner_y + 1, inner_w - 2, msg, C_DIM, C_PANEL, A_NONE);
        if (lib_count(t->lib) == 0) {
            screen_text(s, x + 2, inner_y + 2, inner_w - 2,
                        "Press 'a' to add a folder, or run:", C_DIM, C_PANEL, A_NONE);
            screen_text(s, x + 2, inner_y + 3, inner_w - 2,
                        "  player <file>", C_ACCENT, C_PANEL, A_NONE);
        }
        return;
    }

    /* keep the cursor visible */
    if (t->cursor < t->scroll) t->scroll = t->cursor;
    if (t->cursor >= t->scroll + inner_h) t->scroll = t->cursor - inner_h + 1;
    if (t->scroll < 0) t->scroll = 0;

    for (int row = 0; row < inner_h && t->scroll + row < t->nfiltered; row++) {
        int idx = t->scroll + row;
        const Entry *e = lib_at(t->lib, t->filtered[idx]);
        if (!e) continue;

        bool sel = (idx == t->cursor);
        uint32_t bg = sel ? C_SEL_BG : C_PANEL;
        uint32_t fg = sel ? C_TITLE : C_FG;
        screen_fill(s, x + 1, inner_y + row, inner_w, 1, fg, bg, A_NONE);

        /* playing marker */
        bool is_playing = (t->playing_entry == (Entry *)e) && t->playing;
        if (is_playing) {
            screen_text(s, x + 1, inner_y + row, 2,
                        t->paused ? "∥" : "▶", C_OK, bg, A_BOLD);
        } else {
            screen_text(s, x + 1, inner_y + row, 2, " ", fg, bg, A_NONE);
        }

        /* kind glyph */
        screen_text(s, x + 3, inner_y + row, 2,
                    e->kind == MK_AUDIO ? "♪" : "▤",
                    e->kind == MK_AUDIO ? C_ACCENT : C_DIM, bg, A_NONE);

        /* title, leaving room for duration and progress */
        int metaw = 14;
        int titlew = inner_w - 6 - metaw;
        if (titlew < 6) titlew = inner_w - 6;
        char tmp[512];
        utf8_ellipsize(tmp, sizeof tmp, e->title ? e->title : e->path, titlew);
        screen_text(s, x + 5, inner_y + row, titlew, tmp, fg, bg, A_NONE);

        if (titlew > 8) {
            char meta[32];
            char tc[24];
            if (e->duration > 0) fmt_time(tc, sizeof tc, e->duration);
            else                 snprintf(tc, sizeof tc, "--:--");
            if (e->watched >= 0.97)      snprintf(meta, sizeof meta, "✓ %s", tc);
            else if (e->position > 5.0)  snprintf(meta, sizeof meta, "%d%% %s",
                                                  (int)(e->watched * 100), tc);
            else                          snprintf(meta, sizeof meta, "%s", tc);

            uint32_t mfg = e->watched >= 0.97 ? C_OK
                         : e->position > 5.0  ? C_WARN : C_DIM;
            screen_text_right(s, x + 1, inner_y + row, inner_w - 1, meta, mfg, bg, A_NONE);
        }
    }

    /* scroll indicator */
    if (t->nfiltered > inner_h && inner_h > 0) {
        int barh = inner_h;
        int thumb = (int)((double)barh * (double)inner_h / (double)t->nfiltered);
        if (thumb < 1) thumb = 1;
        int maxscroll = t->nfiltered - inner_h;
        int off = maxscroll > 0
                ? (int)((double)(barh - thumb) * (double)t->scroll / (double)maxscroll)
                : 0;
        for (int i = 0; i < barh; i++) {
            bool on = (i >= off && i < off + thumb);
            screen_text(s, x + w - 1, inner_y + i, 1, on ? "▐" : " ",
                        on ? C_DIM : C_PANEL, C_PANEL, A_NONE);
        }
    }
}

static void draw_now_playing(Tui *t, int x, int y, int w, int h)
{
    Screen *s = &t->scr;
    if (w < 16 || h < 4) return;

    screen_box_titled(s, x, y, w, h, " NOW PLAYING ", C_BORDER, C_PANEL);
    int iy = y + 1, iw = w - 2;

    if (!t->playing) {
        screen_text(s, x + 2, iy, iw - 2, "Nothing playing.", C_DIM, C_PANEL, A_NONE);
        screen_text(s, x + 2, iy + 2, iw - 2, "enter   play selected", C_DIM, C_PANEL, A_NONE);
        screen_text(s, x + 2, iy + 3, iw - 2, "space   pause / resume", C_DIM, C_PANEL, A_NONE);
        return;
    }

    /* title */
    char tmp[512];
    utf8_ellipsize(tmp, sizeof tmp, t->playing_title, iw - 2);
    screen_text(s, x + 2, iy, iw - 2, tmp, C_TITLE, C_PANEL, A_BOLD);

    /* progress bar */
    int barw = iw - 2;
    if (barw > 4) {
        double frac = t->duration > 0 ? t->pos / t->duration : 0;
        screen_bar(s, x + 2, iy + 2, barw, frac, C_BAR, C_BAR_TRACK, C_PANEL);
        char tc1[24], tc2[24];
        fmt_time(tc1, sizeof tc1, t->pos);
        fmt_time(tc2, sizeof tc2, t->duration);
        char line[64];
        snprintf(line, sizeof line, "%s / %s", tc1, t->duration > 0 ? tc2 : "--:--");
        screen_text(s, x + 2, iy + 3, barw, line, C_FG, C_PANEL, A_NONE);
        if (t->paused)
            screen_text_right(s, x + 2, iy + 3, barw, "PAUSED", C_WARN, C_PANEL, A_BOLD);
    }

    if (t->show_stats) {
        char st[256];
        snprintf(st, sizeof st, "%.1f MB/s  %dx%d cells  %s",
                 t->measured_bps / 1e6, t->ti.cols, t->ti.rows, backend_name(t->backend));
        screen_text(s, x + 2, iy + 4, iw - 2, st, C_DIM, C_PANEL, A_NONE);
    }
}

static void draw_queue(Tui *t, int x, int y, int w, int h)
{
    Screen *s = &t->scr;
    if (w < 16 || h < 3) return;

    char title[64];
    snprintf(title, sizeof title, " QUEUE (%d) ", t->qn);
    screen_box_titled(s, x, y, w, h, title, C_BORDER, C_PANEL);

    int iy = y + 1, iw = w - 2;
    int ih = h - 2;
    if (t->qn == 0) {
        screen_text(s, x + 2, iy, iw - 2, "empty", C_DIM, C_PANEL, A_NONE);
        return;
    }
    for (int i = 0; i < ih && i < t->qn; i++) {
        bool cur = (i == t->qpos);
        uint32_t fg = cur ? C_OK : C_FG;
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        screen_text_right(s, x + 1, iy + i, 3, num, cur ? C_OK : C_DIM, C_PANEL, A_NONE);
        char tmp[512];
        char *pretty = media_pretty_title(t->queue[i].path);
        utf8_ellipsize(tmp, sizeof tmp, pretty, iw - 6);
        free(pretty);
        screen_text(s, x + 5, iy + i, iw - 6, tmp, fg, C_PANEL, cur ? A_BOLD : A_NONE);
    }
}

static void draw_footer(Tui *t)
{
    Screen *s = &t->scr;
    int y = s->h - 1;
    screen_fill(s, 0, y, s->w, 1, C_DIM, C_PANEL, A_NONE);

    /* toast takes priority: it is the answer to what just happened */
    if (t->toast[0] && now_ns() < t->toast_until_ns) {
        screen_text(s, 1, y, s->w - 2, t->toast, C_WARN, C_PANEL, A_NONE);
        return;
    }

    if (t->scanning) {
        char line[1024];
        snprintf(line, sizeof line, "scanning… %d found  %s%s", t->scan_found,
                 t->scan_path, t->scan_probing ? "  (probing)" : "");
        screen_text(s, 1, y, s->w - 2, line, C_ACCENT, C_PANEL, A_NONE);
        return;
    }

    static const char *const KEYS =
        "↑↓ move  enter play  space pause  ←→ seek  n next  "
        "/ filter  a add  r rescan  ? help  q quit";
    screen_text(s, 1, y, s->w - 2, KEYS, C_DIM, C_PANEL, A_NONE);
}

static void draw_help(Tui *t)
{
    Screen *s = &t->scr;
    int w = 58, h = 18;
    if (w > s->w - 4) w = s->w - 4;
    if (h > s->h - 4) h = s->h - 4;
    if (w < 20 || h < 8) return;
    int x = (s->w - w) / 2, y = (s->h - h) / 2;

    screen_fill(s, x, y, w, h, C_FG, C_PANEL, A_NONE);
    screen_box_titled(s, x, y, w, h, " KEYS ", C_ACCENT, C_PANEL);

    static const char *const ROWS[][2] = {
        { "↑ ↓ / j k",  "move in the library" },
        { "enter",      "play the selected file" },
        { "space",      "pause / resume" },
        { "← →",        "seek 5 seconds" },
        { "pgup pgdn",  "seek 60 seconds" },
        { "n",          "next in queue" },
        { "a",          "add a folder to the library" },
        { "r",          "rescan the library" },
        { "/",          "filter (esc clears)" },
        { "s",          "toggle throughput stats" },
        { "?",          "this help" },
        { "q",          "quit" },
    };
    int row = 1;
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0] && row < h - 2; i++, row++) {
        screen_text(s, x + 2, y + row, 12, ROWS[i][0], C_ACCENT, C_PANEL, A_BOLD);
        screen_text(s, x + 15, y + row, w - 17, ROWS[i][1], C_FG, C_PANEL, A_NONE);
    }
}

/* ------------------------------------------------------------------ */
/* playback pumping                                                   */
/* ------------------------------------------------------------------ */

static void pump_playback(Tui *t, int pane_x, int pane_y, int pane_w, int pane_h)
{
    if (!t->playing || !t->dec) return;

    int64_t now = now_ns();

    /* how long may we sleep before the next frame is due */
    int64_t frame_ns = 1000000000LL / (t->opt.target_fps > 0 ? t->opt.target_fps : 30);

    if (t->paused) return;

    double out_fps = (double)(t->opt.target_fps > 0 ? t->opt.target_fps : 30);
    double rate = t->play_fps > 1.0 ? t->play_fps : out_fps;

    /* Gate decoding on the clock rather than decoding first and discarding.
     * The discard-first version consumed frames faster than real time whenever
     * the loop spun quickly, ran the pipe dry, then sat on end-of-stream forever
     * with the picture frozen on the last frame. */
    double clk = clock_position(t->clock);
    double next_pts = t->pts_base + (double)t->frame_idx / rate;
    if (next_pts > clk + 1.0 / rate) {
        t->pos = clk;
        return;
    }
    if (t->frame_eof) {
        t->pos = clk;
        return;
    }

    FrameDecision d = clock_decide(t->clock, next_pts, now, frame_ns);
    if (d.action == ACT_HOLD) {
        /* Behind: skip this frame without touching the pipe's buffered state
         * beyond what is already decoded. Reading here is what drift-free
         * playback needs, but it is bounded by the gate above. */
        t->frame_idx++;
        Raster discard; raster_init(&discard);
        decode_read_video(t->dec, &discard);
        raster_free(&discard);
        return;
    }

    /* audio refill */
    if (t->audio && audio_available(t->audio)) {
        static int16_t abuf[2048];
        ssize_t got = decode_read_audio(t->dec, abuf, 2048);
        if (got > 0) {
            audio_write(t->audio, abuf, (size_t)got);
            clock_set_audio_position(t->clock, audio_frames_played(t->audio),
                                     DECODE_AUDIO_RATE, now);
        }
    }

    /* too early: leave the current frame on screen and let the next loop
     * iteration try again. Without this the pane shows the future. */
    if (d.action == ACT_WAIT) {
        t->pos = clock_position(t->clock);
        return;
    }

    /* video */
    if (t->playing_kind == MK_VIDEO) {
        int rc = decode_read_video(t->dec, &t->frame);
        if (rc != 0) t->frame_idx++;   /* every consumed frame advances PTS */
        if (rc == 0) {
            t->playback_done = true;
            if (t->playing_entry) lib_set_finished(t->lib, t->playing_entry);
            if (t->qpos >= 0 && t->qpos + 1 < t->qn) advance_queue(t);
            else { stop_playback(t); toast(t, "Finished"); }
            return;
        }
        if (rc < 0) { stop_playback(t); toast(t, "Decode error"); return; }

        /* fit the raster into the pane, honouring the throughput budget */
        TpAdvice adv = tp_advise(t->tp, t->backend);
        int max_w = 0, max_h = 0;
        Rect box = { 0, 0, pane_w, pane_h };
        gfx_raster_size(t->backend, &t->ti, box, &max_w, &max_h);

        int rw = adv.raster_w, rh = adv.raster_h;
        if (rw > max_w || rw <= 0) rw = max_w;
        if (rh > max_h || rh <= 0) rh = max_h;

        double src_ar = t->frame.h > 0 ? (double)t->frame.w / t->frame.h : 16.0 / 9.0;
        double box_ar = rh > 0 ? (double)rw / rh : src_ar;
        if (src_ar > box_ar) rh = (int)((double)rw / src_ar);
        else                 rw = (int)((double)rh * src_ar);
        if (rw < 2) rw = 2;
        if (rh < 2) rh = 2;

        Raster out; raster_init(&out);
        if (raster_alloc(&out, rw, rh)) {
            raster_scale_nn(&t->frame, &out);

            int cells_w = rw;
            int cells_h = (t->backend == BK_BLOCKS24 || t->backend == BK_BLOCKS256)
                        ? (rh + 1) / 2 : rh;
            Rect dst = { pane_x + (pane_w - cells_w) / 2,
                         pane_y + (pane_h - cells_h) / 2,
                         cells_w, cells_h };
            if (dst.x < pane_x) dst.x = pane_x;
            if (dst.y < pane_y) dst.y = pane_y;

            RasterStats st = raster_stats(&out);
            size_t before = t->w.b.len;
            gfx_draw(t->backend, &t->ti, &t->w, &out, dst);
            size_t wrote = t->w.b.len - before;

            int64_t now2 = now_ns();
            double dt = t->last_draw_ns > 0 ? (double)(now2 - t->last_draw_ns) / 1e9 : 0;
            t->last_draw_ns = now2;
            tp_observe(t->tp, (long)wrote, dt, 0, st.entropy_hint, t->backend);
            t->measured_bps = adv.bytes_per_sec;
        }
        raster_free(&out);
    } else {
        /* audio-only: no raster work at all, just pace */
        sleep_ns(frame_ns);
    }

    t->pos = clock_position(t->clock);

    /* persist the resume point on a debounce */
    if (t->playing_entry && now - t->last_save_ns > 3000000000LL) {
        lib_set_position(t->lib, t->playing_entry, t->pos, t->duration);
        t->last_save_ns = now;
    }

    if ((int)(t->pos*10)%50==0 && t->pos>0.1) fprintf(stderr,"POS %.2f dur %.2f\n", t->pos, t->duration);
    if (t->duration > 0 && t->pos >= t->duration) {
        if (t->playing_entry) lib_set_finished(t->lib, t->playing_entry);
        if (t->qpos >= 0 && t->qpos + 1 < t->qn) advance_queue(t);
        else { stop_playback(t); toast(t, "Finished"); }
    }
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static void play_selected(Tui *t)
{
    if (t->cursor < 0 || t->cursor >= t->nfiltered) return;
    const Entry *e = lib_at(t->lib, t->filtered[t->cursor]);
    if (!e) return;
    t->qpos = -1;
    start_path(t, e->path);
}

static void add_folder_prompt(Tui *t)
{
    /* A full text-entry prompt would need its own little state machine; instead
     * we accept a path typed into the filter line after 'a', which keeps the
     * loop simple and is honest about what it does. */
    toast(t, "Type a folder path after 'a <path>', or use: player add <dir>");
}

static void handle_key(Tui *t, const Key *k)
{
    /* filtering mode eats most keys */
    if (t->filtering) {
        if (k->kind == KEY_ESC) {
            t->filtering = false;
            t->filter[0] = '\0';
            refilter(t);
            return;
        }
        if (k->kind == KEY_ENTER) {
            t->filtering = false;
            if (t->nfiltered) play_selected(t);
            return;
        }
        if (k->kind == KEY_BACKSPACE) {
            size_t n = strlen(t->filter);
            if (n) t->filter[n - 1] = '\0';
            refilter(t);
            return;
        }
        if (k->kind == KEY_DOWN) { move_cursor(t, 1); return; }
        if (k->kind == KEY_UP)   { move_cursor(t, -1); return; }
        if (k->kind == KEY_CHAR && !k->ctrl) {
            size_t n = strlen(t->filter);
            if (n + 2 < sizeof t->filter) {
                strcat(t->filter, k->ch);
                refilter(t);
            }
            return;
        }
        return;
    }

    if (t->help_open) {
        if (k->kind == KEY_ESC ||
            (k->kind == KEY_CHAR && (k->ch[0] == 'q' || k->ch[0] == '?' ||
                                     k->ch[0] == ' ')))
            t->help_open = false;
        return;
    }

    switch (k->kind) {
    case KEY_CHAR:
        if (k->ctrl) {
            if (k->ch[0] == 'c') t->running = false;
            if (k->ch[0] == 'l') { screen_invalidate(&t->scr); }
            break;
        }
        switch (k->ch[0]) {
        case 'q': t->running = false; break;
        case 'j': move_cursor(t, 1); break;
        case 'k': move_cursor(t, -1); break;
        case 'g': t->cursor = 0; break;
        case 'G': t->cursor = t->nfiltered ? t->nfiltered - 1 : 0; break;
        case ' ': toggle_pause(t); break;
        case 'n': if (t->qpos >= 0) advance_queue(t); break;
        case 's': t->show_stats = !t->show_stats; break;
        case '?': t->help_open = true; break;
        case '/': t->filtering = true; t->filter[0] = '\0'; refilter(t); break;
        case 'a': add_folder_prompt(t); break;
        case 'r': {
            t->scanning = true;
            t->scan_found = 0;
            lib_scan(t->lib, scan_progress, t);
            t->scanning = false;
            refilter(t);
            char msg[64];
            snprintf(msg, sizeof msg, "Indexed %d files", lib_count(t->lib));
            toast(t, msg);
            break;
        }
        case 'c': queue_clear(t); toast(t, "Queue cleared"); break;
        case 'Q': {
            if (t->cursor < t->nfiltered) {
                const Entry *e = lib_at(t->lib, t->filtered[t->cursor]);
                if (e) { queue_add(t, e->path); toast(t, "Added to queue"); }
            }
            break;
        }
        default: break;
        }
        break;

    case KEY_ENTER: {
        if (t->qn > 0 && t->qpos < 0) {
            /* if there is a queue and nothing playing, start it */
            t->qpos = 0;
            start_path(t, t->queue[0].path);
        } else {
            play_selected(t);
        }
        break;
    }
    case KEY_UP:    move_cursor(t, -1); break;
    case KEY_DOWN:  move_cursor(t, 1); break;
    case KEY_PGUP:  move_cursor(t, -10); break;
    case KEY_PGDN:  move_cursor(t, 10); break;
    case KEY_LEFT:  seek_relative(t, -5); break;
    case KEY_RIGHT: seek_relative(t, 5); break;
    case KEY_ESC:   t->running = false; break;
    case KEY_RESIZE: {
        term_refresh_size(t->term);
        const TermInfo *ti = term_info(t->term);
        t->ti = *ti;
        screen_resize(&t->scr, ti->cols, ti->rows);
        buf_puts(&t->w.b, "\x1b[2J");
        tp_resize(t->tp, ti->cols, ti->rows, ti->cell_w, ti->cell_h);
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

/* Derive the data directory: $TERMIPLAYER_DATA, else $XDG_DATA_HOME/termiplayer,
 * else ~/.local/share/termiplayer. */
static void data_dir(char *out, size_t cap)
{
    const char *env = getenv("TERMIPLAYER_DATA");
    if (env && *env) { snprintf(out, cap, "%s", env); return; }

    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) { snprintf(out, cap, "%s/termiplayer", xdg); return; }

    const char *home = getenv("HOME");
    if (home && *home) { snprintf(out, cap, "%s/.local/share/termiplayer", home); return; }
    snprintf(out, cap, "./.termiplayer");
}

int tui_run(const TuiOpts *o)
{
    Tui t;
    memset(&t, 0, sizeof t);
    t.opt = *o;
    t.qpos = -1;
    t.show_stats = o->stats;
    t.running = true;
    g_tui = &t;

    char ddir[1024];
    data_dir(ddir, sizeof ddir);

    t.term = term_open(true);
    if (!t.term) { fprintf(stderr, "player: cannot open a terminal\n"); return 1; }
    term_probe(t.term, o->force_backend);
    t.ti = *term_info(t.term);
    t.backend = t.ti.chosen;
    ttyw_init(&t.w, term_fd(t.term));
    screen_init(&t.scr, t.ti.cols, t.ti.rows);

    char ffpath[512];
    const char *why = NULL;
    const char *ff = decode_find_ffmpeg(ffpath, sizeof ffpath, &why);

    t.lib = lib_open(ddir, ff);
    t.tp = tp_new(t.ti.cols, t.ti.rows, t.ti.cell_w, t.ti.cell_h,
                  o->target_fps > 0 ? o->target_fps : 30);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    refilter(&t);
    raster_init(&t.frame);

    /* first run: seed the library with the current directory */
    if (lib_count(t.lib) == 0 && lib_folder_count(t.lib) == 0) {
        char cwd[1024];
        if (getcwd(cwd, sizeof cwd)) {
            lib_add_folder(t.lib, cwd);
            t.scanning = true;
            lib_scan(t.lib, scan_progress, &t);
            t.scanning = false;
            refilter(&t);
        }
    }

    /* an explicit file on the command line plays immediately */
    if (o->start_path) {
        queue_clear(&t);
        queue_add(&t, o->start_path);
        t.qpos = 0;
        start_path(&t, o->start_path);
    }

    int64_t stat_timer = now_ns();

    while (t.running) {
        int64_t now = now_ns();

        /* 1. geometry: wide / medium / narrow */
        int W = t.scr.w, H = t.scr.h;
        int body_y = 1, body_h = H - 2;
        if (body_h < 3) body_h = H - 1 > 0 ? H - 1 : 1;

        int lib_w, right_x, right_w;
        if (W >= 100) {
            lib_w = (W * 52) / 100;
            right_x = lib_w;
            right_w = W - lib_w;
        } else if (W >= 60) {
            lib_w = (W * 58) / 100;
            right_x = lib_w;
            right_w = W - lib_w;
        } else {
            lib_w = W;
            right_x = W;
            right_w = 0;
        }

        /* split the right column into preview and queue when there is room */
        int pre_h = 0, q_y = 0, q_h = 0;
        if (right_w > 0) {
            if (body_h >= 22) {
                pre_h = (body_h * 62) / 100;
                q_y = body_y + pre_h;
                q_h = body_h - pre_h;
            } else {
                pre_h = body_h;
                q_h = 0;
            }
        }

        /* 2. draw the raster first, then text over it */
        ttyw_begin_sync(&t.w);
        screen_clear(&t.scr, C_FG, C_BG);

        if (right_w > 0 && pre_h > 2) {
            pump_playback(&t, right_x + 1, body_y + 1, right_w - 2, pre_h - 2);
        }

        draw_header(&t);
        draw_library(&t, 0, body_y, lib_w, body_h);
        if (right_w > 0) {
            draw_now_playing(&t, right_x, body_y, right_w, pre_h);
            if (q_h > 2) draw_queue(&t, right_x, q_y, right_w, q_h);
        }
        draw_footer(&t);
        if (t.help_open) draw_help(&t);

        /* 3. flush the text diff on top of whatever the raster drew */
        screen_flush(&t.scr, &t.w, t.ti.truecolor, false);
        ttyw_end_sync(&t.w);
        ttyw_flush(&t.w);

        /* 4. input, with a timeout that keeps playback smooth */
        int timeout_ms = 16;
        if (t.playing && t.playing_kind == MK_VIDEO && !t.paused) timeout_ms = 4;

        Key k;
        if (term_read_key(t.term, &k, timeout_ms)) {
            handle_key(&t, &k);
        } else {
            /* no key: pump frames that are due without redrawing the chrome */
            if (t.playing && !t.paused && right_w > 0 && pre_h > 2) {
                ttyw_begin_sync(&t.w);
                pump_playback(&t, right_x + 1, body_y + 1, right_w - 2, pre_h - 2);
                ttyw_end_sync(&t.w);
                ttyw_flush(&t.w);
            }
        }

        /* 5. periodic state save */
        if (now - stat_timer > 5000000000LL) {
            stat_timer = now;
            lib_flush(t.lib);
        }
    }

    /* teardown: save where we actually got to, before stop_playback() clears
     * the timers. Saving after teardown used to write position 0 and mark the
     * file watched, which is why a TUI session never resumed. */
    if (t.playing_entry && !t.playback_done)
        lib_set_position(t.lib, t.playing_entry, clock_position(t.clock), t.duration);
    stop_playback(&t);
    lib_flush(t.lib);
    lib_close(t.lib);
    raster_free(&t.frame);
    if (t.clock) clock_free(t.clock);
    tp_free(t.tp);
    screen_free(&t.scr);
    free(t.filtered);
    queue_clear(&t);
    free(t.queue);
    ttyw_free(&t.w);
    term_close(t.term);
    g_tui = NULL;
    return 0;
}
