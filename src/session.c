/* session.c — the playback loop.
 *
 * Data flow per iteration:
 *
 *   key?  -> handle (seek/pause/quit/speed)
 *   decide clock -> ACT_SHOW / ACT_HOLD / ACT_WAIT
 *   decode frame -> scale to advised raster -> backend emits escapes
 *   observe bytes written -> throughput controller
 *
 * Order matters. Audio is refilled *before* the clock is asked, so the master
 * position is fresh; and the clock is asked before decoding, so a frame we are
 * about to drop never costs a decode.
 */
#include "session.h"

#include "audio.h"
#include "clock.h"
#include "decode.h"
#include "gfx.h"
#include "gfx_internal.h"
#include "library.h"
#include "raster.h"
#include "term.h"
#include "testsrc.h"
#include "throughput.h"
#include "util.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_CHUNK_FRAMES 1024

typedef struct {
    Term       *term;
    TtyWriter   w;
    TermInfo    ti;
    BackendId   backend;
    Rect        area;

    Clock      *clock;
    AudioOut   *audio;
    DecodeSource *dec;

    Raster src;        /* as decoded (or synthetic) */
    Raster out;        /* scaled to the raster we are allowed to draw */

    Throughput *tp;

    bool running;
    bool paused;
    bool video_done;
    bool audio_done;
    bool ends_shown;

    int64_t t0_ns;
    int64_t last_draw_ns;
    int64_t frames_shown;
    int64_t last_stat_ns;
    double  duration;

    SessionOpts opt;

    /* The library is optional here: `player FILE` should remember where you
     * stopped exactly like the TUI does, or "resume" would be a lie for anyone
     * who plays from the shell. Failures are non-fatal. */
    Library *lib;
    Entry   *entry;
    int64_t  last_save_ns;
} Session;

#define AUDIO_RATE DECODE_AUDIO_RATE

/* ------------------------------------------------------------------ */
/* library (optional)                                                 */
/* ------------------------------------------------------------------ */

static void session_data_dir(char *out, size_t cap)
{
    const char *env = getenv("TERMIPLAYER_DATA");
    if (env && *env) { snprintf(out, cap, "%s", env); return; }
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) { snprintf(out, cap, "%s/termiplayer", xdg); return; }
    const char *home = getenv("HOME");
    if (home && *home) { snprintf(out, cap, "%s/.local/share/termiplayer", home); return; }
    snprintf(out, cap, "./.termiplayer");
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* Recompute the drawing area and reset backend state after a resize. */
static void layout(Session *s)
{
    s->area.x = 0;
    s->area.y = 0;
    s->area.w = s->ti.cols;
    /* reserve the bottom 2 rows for the HUD unless quiet */
    s->area.h = s->ti.rows - (s->opt.quiet ? 0 : 2);
    if (s->area.h < 2) { s->area.h = s->ti.rows; }
    tp_resize(s->tp, s->ti.cols, s->ti.rows, s->ti.cell_w, s->ti.cell_h);
}

static void write_hud(Session *s, const char *status)
{
    if (s->opt.quiet) return;

    char pos[24], dur[24];
    double p = clock_position(s->clock);
    fmt_time(pos, sizeof pos, p);
    fmt_time(dur, sizeof dur, s->duration);

    int row = s->ti.rows - 1;
    gfx_cursor(&s->w, row, 1);

    ClockStats cs = clock_stats(s->clock);
    char line[512];
    int col = s->ti.cols;

    /* progress bar width adapts; no bar below 40 columns */
    int barw = col - 60;
    if (barw < 0) barw = 0;
    if (barw > 40) barw = 40;

    int filled = 0;
    if (s->duration > 0 && barw > 0)
        filled = (int)(clampd(p / s->duration, 0, 1) * barw);

    int n = snprintf(line, sizeof line,
                     "\x1b[0m\x1b[7m %s %s%s%s%s%s",
                     s->paused ? "||" : ">",
                     pos, s->duration > 0 ? " / " : "", s->duration > 0 ? dur : "",
                     status ? "  " : "", status ? status : "");
    /* fill the line so an old longer line cannot show through */
    if (n < col) {
        n += snprintf(line + n, sizeof line - (size_t)n, "%*s", col - n, "");
    }
    buf_puts(&s->w.b, line);

    if (barw > 0) {
        gfx_cursor(&s->w, row - 1, 1);
        buf_puts(&s->w.b, "\x1b[0m");
        for (int i = 0; i < barw; i++) {
            if (i < filled) buf_puts(&s->w.b, "\xe2\x96\x88");      /* full block */
            else if (i == filled) buf_puts(&s->w.b, "\xe2\x96\x8c"); /* left half */
            else buf_puts(&s->w.b, "\xe2\x96\x91");                 /* light shade */
        }
    }
    (void)cs;
}

static void write_stats(Session *s)
{
    if (!s->opt.stats) return;
    ClockStats cs = clock_stats(s->clock);
    TpAdvice a = tp_advise(s->tp, s->backend);

    gfx_cursor(&s->w, s->ti.rows, 1);
    buf_printf(&s->w.b,
        "\x1b[0m\x1b[36m[%s] %dx%d cells  raster %dx%d  %.1f MB/s  "
        "av %+.1f ms (avg %+.1f)  shown %ld dropped %ld rep %ld\x1b[0m",
        backend_name(s->backend), s->ti.cols, s->ti.rows,
        a.raster_w, a.raster_h,
        a.bytes_per_sec / 1e6,
        cs.av_diff_ms, cs.av_diff_avg_ms,
        (long)cs.shown, (long)cs.dropped, (long)cs.repeated);
}

/* ------------------------------------------------------------------ */
/* audio pumping                                                      */
/* ------------------------------------------------------------------ */

/* Refill the device with decoded audio. Called every loop iteration; blocking
 * writes here are what pace the whole player. */
static void pump_audio(Session *s, int16_t *buf)
{
    if (!s->audio || s->audio_done || !audio_available(s->audio)) return;
    if (!s->dec) return;

    size_t want = AUDIO_CHUNK_FRAMES * 2;   /* stereo, s16 */
    ssize_t got = decode_read_audio(s->dec, buf, want);
    if (got <= 0) { s->audio_done = true; return; }

    if (!audio_write(s->audio, buf, (size_t)got)) { s->audio_done = true; return; }
    clock_set_audio_position(s->clock, audio_frames_played(s->audio),
                             DECODE_AUDIO_RATE, now_ns());
}

/* ------------------------------------------------------------------ */
/* key handling                                                       */
/* ------------------------------------------------------------------ */

typedef enum { K_CONTINUE, K_QUIT, K_SEEK } KeyResult;

static KeyResult handle_key(Session *s, const Key *k, double *seek_to)
{
    switch (k->kind) {
    case KEY_CHAR:
        if (k->ctrl) break;
        switch (k->ch[0]) {
        case 'q': case 'Q': return K_QUIT;
        case ' ': s->paused = !s->paused;
                  clock_set_paused(s->clock, s->paused, now_ns());
                  if (s->audio) audio_flush(s->audio);
                  break;
        case 'j': break;
        case 'k': break;
        case 'l': case '.': {
            double sp = clock_speed(s->clock) * 2.0;
            clock_set_speed(s->clock, sp);
            break;
        }
        case 'h': case ',': {
            double sp = clock_speed(s->clock) / 2.0;
            clock_set_speed(s->clock, sp);
            break;
        }
        case 's': if (s->opt.stats) s->opt.stats = false; else s->opt.stats = true; break;
        default: break;
        }
        break;

    case KEY_LEFT:
        *seek_to = clock_position(s->clock) - 5.0;
        if (*seek_to < 0) *seek_to = 0;
        return K_SEEK;
    case KEY_RIGHT:
        *seek_to = clock_position(s->clock) + 5.0;
        return K_SEEK;
    case KEY_PGUP:
        *seek_to = clock_position(s->clock) - 60.0;
        if (*seek_to < 0) *seek_to = 0;
        return K_SEEK;
    case KEY_PGDN:
        *seek_to = clock_position(s->clock) + 60.0;
        return K_SEEK;
    case KEY_HOME:
        *seek_to = 0;
        return K_SEEK;
    case KEY_RESIZE: {
        term_refresh_size(s->term);
        const TermInfo *ti = term_info(s->term);
        s->ti.cols = ti->cols;
        s->ti.rows = ti->rows;
        s->ti.cell_w = ti->cell_w;
        s->ti.cell_h = ti->cell_h;
        /* clear the whole screen: stale raster must not survive a resize */
        buf_puts(&s->w.b, "\x1b[2J");
        layout(s);
        break;
    }
    default: break;
    }
    return K_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* main loop                                                          */
/* ------------------------------------------------------------------ */

static void cleanup(Session *s)
{
    if (s->w.fd >= 0) {
        ttyw_begin_sync(&s->w);
        gfx_shutdown(s->backend, &s->w);
        ttyw_end_sync(&s->w);
        ttyw_flush(&s->w);
    }
    ttyw_free(&s->w);
    raster_free(&s->src);
    raster_free(&s->out);
    if (s->dec) decode_close(s->dec);
    if (s->audio) audio_close(s->audio);
    if (s->lib) {
        if (s->entry) {
            double pos = s->clock ? clock_position(s->clock) : 0;
            bool finished = s->duration > 0 && pos >= s->duration - 2.0;
            if (finished) lib_set_finished(s->lib, s->entry);
            else if (pos > 1.0) lib_set_position(s->lib, s->entry, pos, s->duration);
        }
        lib_flush(s->lib);
        lib_close(s->lib);
    }
    clock_free(s->clock);
    tp_free(s->tp);
    if (s->term) term_close(s->term);
}

static void on_signal(int sig);

static Session *g_session;

static void on_signal(int sig)
{
    (void)sig;
    if (g_session) g_session->running = false;
}

int session_run(const SessionOpts *o)
{
    Session s;
    memset(&s, 0, sizeof s);
    s.opt = *o;
    g_session = &s;
    s.running = true;

    /* ---- terminal ---- */
    s.term = term_open(!o->quiet);
    if (!s.term) {
        fprintf(stderr, "player: cannot open a terminal; try --backend ascii > file\n");
        return 1;
    }
    term_probe(s.term, o->force_backend);
    s.ti = *term_info(s.term);
    s.backend = s.ti.chosen;

    ttyw_init(&s.w, term_fd(s.term));

    if (!o->quiet) {
        buf_puts(&s.w.b, "\x1b[2J\x1b[H");
        ttyw_flush(&s.w);
    }

    /* ---- components ---- */
    s.clock = clock_new();
    s.tp = tp_new(s.ti.cols, s.ti.rows, s.ti.cell_w, s.ti.cell_h,
                  s.ti.px_w > 0 ? 30 : o->target_fps);
    if (o->force_w > 0 && o->force_h > 0) tp_force_raster(s.tp, o->force_w, o->force_h);
    /* layout() touches the throughput controller, so it must come after it. */
    layout(&s);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int16_t *abuf = xmalloc(AUDIO_CHUNK_FRAMES * 2 * sizeof(int16_t));

    /* ---- open the source ---- */
    MediaInfo mi;
    memset(&mi, 0, sizeof mi);

    if (o->testsrc) {
        s.duration = o->testsrc_seconds > 0 ? o->testsrc_seconds : 10;
        s.video_done = false;
    } else {
        char err[512];
        bool want_v = !o->no_video;
        bool want_a = !o->no_audio;
        s.dec = decode_open(o->path, want_v, want_a, &mi, err, sizeof err);
        if (!s.dec) {
            cleanup(&s);
            fprintf(stderr, "player: %s\n", err);
            return 1;
        }
        s.duration = mi.duration;

        AudioInfo ai;
        if (mi.has_audio && !o->no_audio) {
            s.audio = audio_open(DECODE_AUDIO_RATE, 2, &ai);
            if (!audio_available(s.audio) && !o->quiet)
                warnx_("no audio device: %s", ai.note);
        }
    }

    /* ---- library: resume support for the one-shot path ---- */
    if (!o->testsrc && o->path && !o->no_resume) {
        char ddir[1024];
        session_data_dir(ddir, sizeof ddir);
        s.lib = lib_open(ddir, mi.ffmpeg_path);
        if (s.lib) s.entry = lib_touch(s.lib, o->path);
    }

    /* ---- clock ---- */
    double resume_at = o->start_at;
    if (resume_at <= 0 && s.entry && s.entry->position > 5.0 &&
        (s.duration <= 0 || s.entry->position < s.duration - 10.0)) {
        resume_at = s.entry->position;
        if (!o->quiet) {
            gfx_cursor(&s.w, s.ti.rows, 1);
            buf_printf(&s.w.b, "\x1b[0m\x1b[2mresuming at %.0f s\x1b[0m", resume_at);
            ttyw_flush(&s.w);
        }
    }
    /* Order matters: start_monotonic zeroes the clock, so any seek must come
     * after it. Getting this backwards silently disabled resume. */
    clock_start_monotonic(s.clock, now_ns());
    if (resume_at > 0) {
        clock_seek(s.clock, resume_at, now_ns());
        if (s.dec) {
            char serr[256];
            if (decode_seek(s.dec, resume_at, serr, sizeof serr) != 0)
                warnx_("could not resume: %s", serr);
        }
    }
    /* Initialise the audio reference at the position we actually start from.
     * Passing 0 here unconditionally was the second half of the resume bug: it
     * ran after the seek and dragged the clock back to zero. */
    if (s.audio && audio_available(s.audio))
        clock_set_audio_position(s.clock,
                                 (int64_t)(resume_at * (double)DECODE_AUDIO_RATE),
                                 DECODE_AUDIO_RATE, now_ns());

    s.t0_ns = now_ns();
    s.last_stat_ns = s.t0_ns;

    /* ---- the loop ---- */
    /* The presentation timestamp of a decoded frame is its index divided by the
     * source frame rate. Using the clock position as the PTS made every frame
     * look perfectly in sync (diff == 0), so the pacer never waited and the
     * whole clip decoded as fast as ffmpeg could produce it. */
    double play_fps = mi.src_fps > 1.0 ? mi.src_fps : 0.0;
    int frame_no = 0;
    /* Where the current decode stream starts. After a resume or a seek the
     * first frame out of ffmpeg is at that timestamp, not at zero, so PTS has to
     * be offset by it -- otherwise the pacer re-bases the clock to 0 and the
     * resume is silently undone one loop later. */
    double pts_base = resume_at > 0 ? resume_at : 0.0;
    const double start_pos = pts_base;
    double seek_to = -1;

    while (s.running) {
        int64_t now = now_ns();

        /* 1. input (non-blocking) */
        Key k;
        while (term_read_key(s.term, &k, 0)) {
            double target = -1;
            KeyResult r = handle_key(&s, &k, &target);
            if (r == K_QUIT) { s.running = false; break; }
            if (r == K_SEEK) seek_to = target;
            now = now_ns();
        }
        if (!s.running) break;

        /* 2. seeks */
        if (seek_to >= 0) {
            clock_seek(s.clock, seek_to, now_ns());
            clock_set_paused(s.clock, false, now_ns());
            s.paused = false;
            pts_base = seek_to;
            frame_no = 0;
            if (s.audio) audio_flush(s.audio);
            if (s.dec) {
                char err[256];
                if (decode_seek(s.dec, seek_to, err, sizeof err) != 0) {
                    warnx_("seek failed: %s", err);
                } else {
                    s.video_done = false;
                    s.audio_done = false;
                }
            }
            seek_to = -1;
            now = now_ns();
        }

        /* 3. audio first, so the clock is fresh */
        pump_audio(&s, abuf);
        if (s.audio && audio_available(s.audio))
            clock_set_audio_position(s.clock, audio_frames_played(s.audio), DECODE_AUDIO_RATE, now_ns());

        /* 4. duration limit / end conditions */
        double pos = clock_position(s.clock);
        /* -d means "play for N seconds", not "stop at timestamp N", so it is
         * measured from wherever playback started. Comparing it against the
         * absolute position made "-t 30 -d 5" exit before the first frame. */
        if (s.opt.duration_limit > 0 && pos >= start_pos + s.opt.duration_limit) break;
        if (s.duration > 0 && pos >= s.duration + 0.5) break;

        /* 5. pacing: how long may we sleep before the next frame is due? */
        int64_t frame_ns = 1000000000LL / (o->target_fps > 0 ? o->target_fps : 30);

        if (s.paused) {
            write_hud(&s, "paused");
            ttyw_begin_sync(&s.w);
            ttyw_flush(&s.w);
            sleep_ns(20000000);
            continue;
        }

        /* 6. decide whether this frame is worth drawing */
        double out_fps = (double)(o->target_fps ? o->target_fps : 30);
        /* PTS must come from the *content*, not from the clock: the clock is
         * what we are trying to synchronise against. */
        double pts;
        if (s.dec) {
            double rate = play_fps > 0 ? play_fps : out_fps;
            pts = pts_base + (double)frame_no / rate;
        } else {
            pts = (double)frame_no / out_fps;
        }
        FrameDecision d = clock_decide(s.clock, pts, now, frame_ns);

        if (d.action == ACT_HOLD) {
            /* behind: drop. Still must consume the decoded frame to stay in
             * sync with the pipe, so decode and discard. */
            if (s.dec) { Raster tmp; raster_init(&tmp); decode_read_video(s.dec, &tmp); raster_free(&tmp); }
            frame_no++;
            continue;
        }
        if (d.action == ACT_WAIT && d.wait_ns > 0) {
            /* Wait, but stay responsive: sleep in 5 ms slices checking input. */
            int64_t deadline = now_ns() + d.wait_ns;
            while (now_ns() < deadline && s.running) {
                Key kk;
                if (term_read_key(s.term, &kk, 5)) {
                    double target = -1;
                    KeyResult r = handle_key(&s, &kk, &target);
                    if (r == K_QUIT) { s.running = false; }
                    if (r == K_SEEK) seek_to = target;
                }
            }
            if (!s.running) break;
            if (seek_to >= 0) continue;
        }

        /* 7. get a frame */
        bool have_frame = false;
        int64_t t_decode0 = now_ns();

        if (o->testsrc) {
            double phase = (double)(now_ns() - s.t0_ns) / 1e9;
            if (s.duration > 0 && phase > s.duration) break;
            if (!raster_alloc(&s.src, 320, 180)) break;
            testsrc_render(&s.src, frame_no, phase);
            have_frame = true;
        } else if (s.dec) {
            int r = decode_read_video(s.dec, &s.src);
            if (r == 0) {
                s.video_done = true;
                if (s.audio_done || !s.audio) break;
            } else if (r < 0) {
                break;
            } else {
                have_frame = true;
            }
        }
        double decode_ms = (double)(now_ns() - t_decode0) / 1e6;

        if (!have_frame) {
            sleep_ns(frame_ns);
            frame_no++;
            continue;
        }

        /* 8. ask the controller what raster we may use, then scale */
        TpAdvice adv = tp_advise(s.tp, s.backend);
        if (o->quiet) { adv.raster_w = s.area.w; adv.raster_h = s.area.h; }

        /* fit inside the area, preserving the source aspect ratio */
        int rw = adv.raster_w, rh = adv.raster_h;
        double src_ar = s.src.h > 0 ? (double)s.src.w / (double)s.src.h : 1.0;
        double box_ar = (double)rw / (double)rh;
        if (src_ar > box_ar) rh = (int)((double)rw / src_ar);
        else                  rw = (int)((double)rh * src_ar);
        if (rw < 2) rw = 2;
        if (rh < 2) rh = 2;

        /* The rectangle the backend draws into, in cells. Raster pixels map to
         * cells differently per backend: blocks use 1 px wide x 2 px tall per
         * cell, so the cell box is exactly the raster size (rounded up); the
         * pixel backends are handed a cell box and work out their own pixels. */
        Rect target = { 0, 0, rw, rh };
        int area_cell_w = rw;
        int area_cell_h = (s.backend == BK_BLOCKS24 || s.backend == BK_BLOCKS256)
                          ? (rh + 1) / 2 : rh;
        int cx = (s.area.w - area_cell_w) / 2;
        int cy = (s.area.h - area_cell_h) / 2;
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        target.x = cx;
        target.y = cy;
        target.w = area_cell_w;
        target.h = area_cell_h;

        if (!raster_alloc(&s.out, rw, rh)) break;
        raster_scale_nn(&s.src, &s.out);

        /* 9. draw */
        RasterStats st = raster_stats(&s.out);
        ttyw_begin_sync(&s.w);
        if (gfx_needs_clear(s.backend)) {
            buf_puts(&s.w.b, "\x1b[2J");
            gfx_clear(s.backend, &s.w, target);
        }
        gfx_draw(s.backend, &s.ti, &s.w, &s.out, target);
        write_stats(&s);
        long written = (long)s.w.b.len;
        ttyw_end_sync(&s.w);

        ttyw_flush(&s.w);

        int64_t now_draw = now_ns();
        double frame_dt = s.last_draw_ns > 0
                        ? (double)(now_draw - s.last_draw_ns) / 1e9 : 0.0;
        s.last_draw_ns = now_draw;

        tp_observe(s.tp, written, frame_dt, decode_ms, st.entropy_hint, s.backend);

        frame_no++;
        s.frames_shown++;

        /* 9b. persist the resume point, debounced */
        if (s.entry && now - s.last_save_ns > 3000000000LL) {
            lib_set_position(s.lib, s.entry, clock_position(s.clock), s.duration);
            s.last_save_ns = now;
        }

        /* 10. HUD, cheap and only when it changed meaningfully */
        if (!o->quiet && (s.frames_shown % 10) == 0) {
            ttyw_begin_sync(&s.w);
            write_hud(&s, NULL);
            ttyw_end_sync(&s.w);
            ttyw_flush(&s.w);
        }
    }

    /* ---- teardown ---- */
    free(abuf);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    cleanup(&s);
    g_session = NULL;
    return 0;
}
