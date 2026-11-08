/* main.c — CLI surface and subcommands.
 *
 * The command line is the product's front door, so it is deliberately small and
 * conventional:
 *
 *   player FILE [options]     play
 *   player testsrc            prove the render path, no media needed
 *   player probe              what did we detect, and why
 *   player doctor             diagnosis + what to fix
 *   player backends           list the ladder, cheapest-to-most-compatible
 */
#include "audio.h"
#include "decode.h"
#include "gfx.h"
#include "session.h"
#include "term.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION "0.1.0"

static void usage(FILE *f)
{
    fputs(
"player " VERSION " - a terminal media player\n"
"\n"
"USAGE\n"
"  player [FILE] [options]     play a video or audio file\n"
"  player testsrc [options]    play a synthetic test pattern (no media needed)\n"
"  player probe                show terminal capabilities and the backend ladder\n"
"  player doctor               diagnose playback problems and say what to fix\n"
"  player backends             list every backend, best first\n"
"\n"
"OPTIONS\n"
"  -b, --backend NAME   force a backend: kitty-shm kitty iterm2 sixel\n"
"                       blocks blocks256 ascii      (default: auto)\n"
"  -s, --size WxH       force the pixel raster (e.g. -s 160x90)\n"
"  -t, --at TIME        start at TIME (seconds, or 1:23:45)\n"
"  -d, --duration SEC   stop after SEC seconds\n"
"      --fps N          target frame rate (default 30)\n"
"      --no-audio       decode video only\n"
"      --no-video       decode audio only\n"
"      --loop           restart when the file ends\n"
"      --stats          show a live statistics line\n"
"  -q, --quiet          no HUD, no alternate screen (for pipes and scripts)\n"
"  -h, --help           this text\n"
"  -V, --version        version\n"
"\n"
"KEYS\n"
"  space pause      left/right seek 5s      pgup/pgdn seek 60s\n"
"  h/l   speed /2 or *2                     s  toggle stats\n"
"  q     quit\n"
"\n"
"ENVIRONMENT\n"
"  TERMIPLAYER_FFMPEG    path to the ffmpeg binary to use\n"
"  TERMIPLAYER_AUDIO_FD  write PCM to this fd instead of opening a device\n"
"  TERMIPLAYER_DEBUG     keep ffmpeg's stderr visible\n"
, f);
}

/* ------------------------------------------------------------------ */
/* probe / doctor                                                     */
/* ------------------------------------------------------------------ */

static void print_probe(void)
{
    Term *t = term_open(false);
    if (!t) { fprintf(stderr, "player: no terminal available\n"); return; }
    term_probe(t, "auto");
    const TermInfo *ti = term_info(t);

    printf("terminal\n");
    printf("  TERM            %s%s\n", ti->term[0] ? ti->term : "(unset)",
           !strcmp(ti->term, "dumb") ? "   <- not a real terminal" : "");
    printf("  TERM_PROGRAM    %s\n", ti->term_program[0] ? ti->term_program : "(unset)");
    printf("  COLORTERM       %s\n", ti->colorterm[0] ? ti->colorterm : "(unset)");
    printf("  is a tty        %s\n", ti->is_tty ? "yes" : "no");
    printf("  size            %dx%d cells", ti->cols, ti->rows);
    if (ti->cell_w > 0) printf("  (%dx%d px per cell, %dx%d px window)",
                               ti->cell_w, ti->cell_h, ti->px_w, ti->px_h);
    else printf("  (cell pixel size unknown)");
    printf("\n");
    if (ti->in_tmux || ti->in_screen || ti->in_zellij)
        printf("  multiplexer     %s\n",
               ti->in_tmux ? "tmux" : ti->in_zellij ? "zellij" : "screen");

    printf("\ncapabilities\n");
    printf("  truecolor       %s\n", ti->truecolor ? "yes" : "no");
    printf("  kitty graphics  %s\n", ti->kitty_protocol ? "yes" : "no");
    printf("  iTerm2 images   %s\n", ti->iterm2_images ? "yes" : "no");
    printf("  sixel           %s\n", ti->sixel ? "yes" : "no");
    printf("  zlib (runtime)  %s\n", zlib_available() ? "yes" : "no");

    printf("\nbackend ladder\n");
    for (int i = 0; i < ti->n_verdicts; i++) {
        const BackendVerdict *v = &ti->verdicts[i];
        printf("  %-11s %s  %s\n", backend_name(v->id),
               v->available ? "[ok]    " : "[skip]  ", v->reason);
    }
    printf("\nchosen: %s\n", backend_name(ti->chosen));
    term_close(t);
}

static void print_doctor(void)
{
    printf("player doctor\n=============\n\n");

    /* 1. ffmpeg */
    char buf[512];
    const char *why = NULL;
    const char *ff = decode_find_ffmpeg(buf, sizeof buf, &why);
    printf("decode backend\n");
    if (ff) printf("  [ok]   ffmpeg at %s\n", ff);
    else    printf("  [FAIL] %s\n         video and audio cannot be decoded.\n", why);

    /* 2. audio */
    AudioInfo ai;
    AudioOut *a = audio_open(48000, 2, &ai);
    printf("\naudio output\n");
    if (audio_available(a)) printf("  [ok]   %s: %s\n", ai.backend, ai.note);
    else printf("  [warn] %s\n         playback still works, silently.\n", ai.note);
    audio_close(a);

    /* 3. terminal */
    printf("\nterminal\n");
    Term *t = term_open(false);
    if (!t) {
        printf("  [warn] no controlling terminal; use --quiet for scripting\n");
    } else {
        term_probe(t, "auto");
        const TermInfo *ti = term_info(t);
        printf("  [ok]   %s %dx%d cells, backend %s\n",
               ti->term[0] ? ti->term : "(unset)", ti->cols, ti->rows,
               backend_name(ti->chosen));
        if (!ti->truecolor && ti->chosen >= BK_BLOCKS24)
            printf("         no truecolor: output will use 256 colours.\n"
                   "         If your terminal does support it, set COLORTERM=truecolor.\n");
        if (ti->cell_w == 0 && ti->chosen < BK_BLOCKS24)
            printf("         cell pixel size unknown: images may be scaled by the\n"
                   "         terminal rather than by us. Use -s WxH to force a raster.\n");
        if (ti->in_tmux)
            printf("         inside tmux: graphics need 'set -g allow-passthrough on'\n"
                   "         (tmux 3.3+). Without it we fall back to blocks.\n");
        if (ti->in_zellij)
            printf("         inside zellij: image support depends on the version;\n"
                   "         blocks are the reliable choice.\n");
        if (!strcmp(ti->term, "dumb"))
            printf("         TERM=dumb: only ASCII output is possible here.\n");
        term_close(t);
    }

    printf("\nsummary\n");
    printf("  if playback stutters, try:  player FILE --stats\n"
           "  to force a smaller picture:  player FILE -s 120x68\n"
           "  to force a backend:          player FILE -b blocks\n");
}

static void print_backends(void)
{
    printf("backend ladder (best first)\n\n");
    static const char *const rows[] = {
        "kitty-shm  real pixels   ~64 B/frame    kitty, local session",
        "kitty      real pixels   compressed     kitty, Ghostty, WezTerm",
        "iterm2     real pixels   uncompressed   iTerm2, WezTerm",
        "sixel      real pixels   ~70x cheaper   foot, contour, mlterm, WT 1.22+",
        "blocks     half blocks   ~37 B/cell     any truecolor terminal",
        "blocks256  half blocks   ~19 B/cell     256-colour terminals",
        "ascii      text          least data     always works",
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
        printf("  %s\n", rows[i]);
    printf("\nbytes-per-frame figures are measured; see spikes/RESULTS.md\n");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    SessionOpts o;
    memset(&o, 0, sizeof o);
    o.force_backend = "auto";
    o.target_fps = 30;

    const char *file = NULL;
    bool want_probe = false, want_doctor = false, want_backends = false;
    bool want_help = false, want_version = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { want_help = true; continue; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { want_version = true; continue; }
        if (!strcmp(a, "probe")) { want_probe = true; continue; }
        if (!strcmp(a, "doctor")) { want_doctor = true; continue; }
        if (!strcmp(a, "backends")) { want_backends = true; continue; }
        if (!strcmp(a, "testsrc")) { o.testsrc = true; continue; }

        if (!strcmp(a, "-b") || !strcmp(a, "--backend")) {
            if (!next) die("%s needs a value", a);
            o.force_backend = next; i++; continue;
        }
        if (!strcmp(a, "-s") || !strcmp(a, "--size")) {
            if (!next) die("%s needs WxH", a);
            int w = 0, h = 0;
            if (sscanf(next, "%dx%d", &w, &h) != 2 || w < 2 || h < 2)
                die("bad size '%s' (expected WxH, e.g. 160x90)", next);
            o.force_w = w; o.force_h = h; i++; continue;
        }
        if (!strcmp(a, "-t") || !strcmp(a, "--at")) {
            if (!next) die("%s needs a time", a);
            double t = parse_timecode(next);
            if (t < 0) die("bad time '%s'", next);
            o.start_at = t; i++; continue;
        }
        if (!strcmp(a, "-d") || !strcmp(a, "--duration")) {
            if (!next) die("%s needs seconds", a);
            o.duration_limit = atof(next); i++; continue;
        }
        if (!strcmp(a, "--fps")) {
            if (!next) die("%s needs a number", a);
            o.target_fps = atoi(next);
            if (o.target_fps < 1 || o.target_fps > 240) die("fps out of range");
            i++; continue;
        }
        if (!strcmp(a, "--no-audio")) { o.no_audio = true; continue; }
        if (!strcmp(a, "--no-video")) { o.no_video = true; continue; }
        if (!strcmp(a, "--loop"))     { o.loop = true; continue; }
        if (!strcmp(a, "--stats"))    { o.stats = true; continue; }
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) { o.quiet = true; continue; }

        if (a[0] == '-' && a[1] != '\0') die("unknown option '%s' (try --help)", a);
        if (!file) file = a;
        else die("more than one file given ('%s' and '%s')", file, a);
    }

    if (want_version) { printf("player %s\n", VERSION); return 0; }
    if (want_help) { usage(stdout); return 0; }
    if (want_probe) { print_probe(); return 0; }
    if (want_doctor) { print_doctor(); return 0; }
    if (want_backends) { print_backends(); return 0; }

    if (!o.testsrc && !file) { usage(stderr); return 2; }
    if (o.testsrc) { o.testsrc_seconds = 10; o.no_audio = true; }
    o.path = file;

    return session_run(&o);
}
