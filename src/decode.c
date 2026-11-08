/* decode.c — the ffmpeg-process decoder.
 *
 * Two child processes (one per stream) when both are wanted, so audio is never
 * blocked behind a slow video frame. Each pipe is read with blocking reads in
 * its own thread by the caller; start-up failures surface as a zero-length
 * first read, which we convert into a useful error message rather than a hang.
 */
#include "decode.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct DecodeSource {
    pid_t  pid_v, pid_a;
    int    fd_v, fd_a;
    int    w, h;
    bool   ended_v, ended_a;
    size_t frame_bytes;
    int    audio_rate;      /* the rate we resample to (fixed) */
    char  *path;
};

/* ------------------------------------------------------------------ */
/* locating a binary                                                  */
/* ------------------------------------------------------------------ */

static bool is_exec(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
}

const char *decode_find_ffmpeg(char *buf, size_t cap, const char **why)
{
    const char *env = getenv("TERMIPLAYER_FFMPEG");
    if (env && *env) {
        if (is_exec(env)) { snprintf(buf, cap, "%s", env); return buf; }
        if (why) *why = "TERMIPLAYER_FFMPEG is set but not executable";
    }

    /* PATH */
    const char *path = getenv("PATH");
    if (path) {
        const char *p = path;
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t n = colon ? (size_t)(colon - p) : strlen(p);
            if (n && n + 8 < cap) {
                snprintf(buf, cap, "%.*s/ffmpeg", (int)n, p);
                if (is_exec(buf)) return buf;
            }
            if (!colon) break;
            p = colon + 1;
        }
    }

    /* Well-known locations, including the imageio-ffmpeg layout used in
     * containers and virtualenvs, which is where this was developed. */
    static const char *const fixed[] = {
        "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/opt/homebrew/bin/ffmpeg",
        "/snap/bin/ffmpeg", "/data/data/com.termux/files/usr/bin/ffmpeg",
        NULL
    };
    for (int i = 0; fixed[i]; i++)
        if (is_exec(fixed[i])) { snprintf(buf, cap, "%s", fixed[i]); return buf; }

    if (why) *why = "ffmpeg not found on PATH (install ffmpeg, or set TERMIPLAYER_FFMPEG)";
    return NULL;
}

/* ------------------------------------------------------------------ */
/* spawn                                                              */
/* ------------------------------------------------------------------ */

/* Execute ffmpeg with the given args, returning a read fd on its stdout.
 * stdin is /dev/null so ffmpeg can never block on a prompt. stderr goes to
 * /dev/null: writing ffmpeg's chatter onto our terminal would corrupt frames.
 * Set TERMIPLAYER_DEBUG=1 to keep stderr for diagnosis. */
static int spawn_ffmpeg(const char *ffmpeg, char *const argv[], pid_t *pid_out,
                        char *err, size_t errcap)
{
    int fds[2];
    if (pipe(fds) != 0) {
        snprintf(err, errcap, "pipe: %s", strerror(errno));
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errcap, "fork: %s", strerror(errno));
        close(fds[0]); close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (!getenv("TERMIPLAYER_DEBUG")) dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        execv(ffmpeg, argv);
        _exit(127);   /* exec failed */
    }

    close(fds[1]);
    /* A big pipe buffer would let ffmpeg run ahead; the kernel default (64K) is
     * a reasonable cushion and keeps memory flat. */
    *pid_out = pid;
    return fds[0];
}

/* ------------------------------------------------------------------ */
/* probing                                                            */
/* ------------------------------------------------------------------ */

/* Ask ffmpeg for the stream layout. We parse ffmpeg's own banner because we
 * deliberately avoid an ffprobe dependency (many installs have one, not both). */
static void probe_media(const char *ffmpeg, const char *path, MediaInfo *mi)
{
    int fds[2];
    if (pipe(fds) != 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }

    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        close(fds[0]); close(fds[1]);
        char *argv[] = {
            (char *)ffmpeg, (char *)"-hide_banner", (char *)"-i", (char *)path, NULL
        };
        execv(ffmpeg, argv);
        _exit(127);
    }
    close(fds[1]);

    Buf out; buf_init(&out);
    char tmp[4096];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof tmp)) > 0) buf_append(&out, tmp, (size_t)n);
    close(fds[0]);
    waitpid(pid, NULL, 0);
    buf_putc(&out, '\0');

    const char *s = out.p ? out.p : "";

    /* "Duration: 00:01:23.45" */
    const char *dur = strstr(s, "Duration: ");
    if (dur) {
        int hh = 0, mm = 0; double ss = 0;
        if (sscanf(dur + 10, "%d:%d:%lf", &hh, &mm, &ss) == 3)
            mi->duration = hh * 3600.0 + mm * 60.0 + ss;
    }

    /* "Stream #0:0: Video: h264 ..., 1280x720 [SAR ...], 30 fps" */
    const char *v = strstr(s, "Video: ");
    if (v) {
        mi->has_video = true;
        for (const char *p = v; *p && p < v + 2000; p++) {
            int w = 0, h = 0;
            if (sscanf(p, "%dx%d", &w, &h) == 2 && w >= 16 && h >= 16 && w < 16384) {
                /* reject things that look like bitrates/other numbers */
                if (p > v && (p[-1] == ' ' || p[-1] == ',')) {
                    mi->src_w = w; mi->src_h = h; break;
                }
            }
        }
        const char *fps = strstr(v, " fps");
        if (fps) {
            /* walk back to the number, may be fractional like "29.97" */
            const char *p = fps;
            int steps = 0;
            while (p > v && steps++ < 24 && p[-1] != ' ' && p[-1] != ',') p--;
            double f = atof(p);
            if (f > 1 && f < 1000) mi->src_fps = f;
        }
    }

    /* "Stream #0:1: Audio: aac ..., 48000 Hz, stereo" */
    const char *a = strstr(s, "Audio: ");
    if (a) {
        mi->has_audio = true;
        const char *hz = strstr(a, " Hz");
        if (hz) {
            const char *p = hz;
            while (p > a && (p[-1] >= '0' && p[-1] <= '9')) p--;
            int r = atoi(p);
            if (r > 4000 && r < 400000) mi->audio_rate = r;
        }
        mi->audio_channels = strstr(a, "mono") ? 1 : 2;
    }

    buf_free(&out);
}

/* ------------------------------------------------------------------ */
/* open                                                               */
/* ------------------------------------------------------------------ */

DecodeSource *decode_open(const char *path, bool want_video, bool want_audio,
                          MediaInfo *out, char *err, size_t errcap)
{
    memset(out, 0, sizeof *out);
    out->audio_rate = 48000;
    out->audio_channels = 2;

    const char *why = NULL;
    if (!decode_find_ffmpeg(out->ffmpeg_path, sizeof out->ffmpeg_path, &why)) {
        snprintf(err, errcap, "%s", why);
        return NULL;
    }

    probe_media(out->ffmpeg_path, path, out);
    if (!out->has_video && !out->has_audio) {
        snprintf(err, errcap, "no playable audio or video stream in '%s'", path);
        return NULL;
    }
    if (want_video && !out->has_video) want_video = false;
    if (want_audio && !out->has_audio) want_audio = false;
    if (!want_video && !want_audio) {
        snprintf(err, errcap, "'%s' has no %s stream", path,
                 out->has_video ? "audio" : "video");
        return NULL;
    }

    DecodeSource *d = xcalloc(1, sizeof *d);
    d->fd_v = d->fd_a = -1;
    d->path = xstrdup(path);
    d->audio_rate = DECODE_AUDIO_RATE;   /* what we ask ffmpeg to produce */
    d->w = out->src_w > 0 ? out->src_w : 640;
    d->h = out->src_h > 0 ? out->src_h : 360;
    d->frame_bytes = (size_t)d->w * (size_t)d->h * DECODE_BPP;

    if (want_video) {
        /* Build argv explicitly. Getting this wrong silently breaks ffmpeg (and
         * with stderr hidden, silently produces an empty pipe), so the argument
         * list is assembled one entry at a time and asserted to be NULL-safe. */
        char *vf = xmalloc(64);
        snprintf(vf, 64, "scale=%dx%d", d->w, d->h);

        const char *a[] = {
            out->ffmpeg_path,
            "-hide_banner", "-loglevel", "error", "-nostdin",
            "-i", path,
            "-an", "-sn", "-dn",
            "-vf", vf,
            "-pix_fmt", "rgb24",
            "-f", "rawvideo",
            "-",
        };
        size_t n = sizeof a / sizeof a[0];
        char **argv = xcalloc(n + 1, sizeof *argv);
        for (size_t i = 0; i < n; i++) argv[i] = (char *)a[i];

        d->fd_v = spawn_ffmpeg(out->ffmpeg_path, argv, &d->pid_v, err, errcap);
        free(argv);
        free(vf);
        if (d->fd_v < 0) { decode_close(d); return NULL; }
    }

    if (want_audio) {
        /* Always resample to the rate the clock assumes, so the audio master
         * position and our frame arithmetic cannot disagree. */
        char rate[32];
        snprintf(rate, sizeof rate, "%d", d->audio_rate);
        const char *a[] = {
            out->ffmpeg_path,
            "-hide_banner", "-loglevel", "error", "-nostdin",
            "-i", path,
            "-vn", "-sn", "-dn",
            "-ac", "2",
            "-ar", rate,
            "-f", "s16le",
            "-",
        };
        size_t n = sizeof a / sizeof a[0];
        char **argv = xcalloc(n + 1, sizeof *argv);
        for (size_t i = 0; i < n; i++) argv[i] = (char *)a[i];

        d->fd_a = spawn_ffmpeg(out->ffmpeg_path, argv, &d->pid_a, err, errcap);
        free(argv);
        if (d->fd_a < 0 && d->fd_v >= 0) decode_kill(d);   /* audio optional */
    }

    return d;
}

void decode_kill(DecodeSource *d)
{
    if (!d) return;
    if (d->pid_v > 0) { kill(d->pid_v, SIGKILL); d->pid_v = -1; }
    if (d->pid_a > 0) { kill(d->pid_a, SIGKILL); d->pid_a = -1; }
    if (d->fd_v >= 0) { close(d->fd_v); d->fd_v = -1; }
    if (d->fd_a >= 0) { close(d->fd_a); d->fd_a = -1; }
}

/* Wait for a child with a bounded timeout, then escalate.
 *
 * The order here matters and is easy to get wrong: close the pipes BEFORE
 * signalling. ffmpeg writes to stdout with blocking writes; if we SIGTERM it
 * while a pipe is full and nobody is draining, it can never finish its muxer
 * teardown and never exits, so waitpid() blocks forever. Closing the read end
 * first gives it EPIPE, which it handles immediately. */
static void reap(pid_t pid, int *fd)
{
    if (fd && *fd >= 0) { close(*fd); *fd = -1; }
    if (pid <= 0) return;

    kill(pid, SIGTERM);
    for (int i = 0; i < 30; i++) {          /* up to 300 ms of grace */
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        sleep_ns(10000000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

void decode_close(DecodeSource *d)
{
    if (!d) return;
    reap(d->pid_v, &d->fd_v);
    reap(d->pid_a, &d->fd_a);
    free(d->path);
    free(d);
}

/* ------------------------------------------------------------------ */
/* reading                                                            */
/* ------------------------------------------------------------------ */

/* Read exactly n bytes, but never block forever: a dead or wedged ffmpeg must
 * surface as an error, not as a hung player. The timeout is generous (it is a
 * decoder stall, not a network read) and the total is bounded by frame size. */
#define READ_STALL_TIMEOUT_MS 8000

static ssize_t read_full(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;
    while (got < n) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, READ_STALL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) return -1;             /* decoder stalled */
        ssize_t k = read(fd, p + got, n - got);
        if (k < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (k == 0) break;                  /* EOF */
        got += (size_t)k;
    }
    return (ssize_t)got;
}

int decode_read_video(DecodeSource *d, Raster *r)
{
    if (!d || d->fd_v < 0 || d->ended_v) return 0;
    if (!raster_alloc(r, d->w, d->h)) return -1;

    ssize_t got = read_full(d->fd_v, r->px, d->frame_bytes);
    if (got < 0) return -1;
    if (got == 0) { d->ended_v = true; return 0; }
    if ((size_t)got < d->frame_bytes) { d->ended_v = true; return 0; }

    /* White bar to show end-of-stream trimming: not needed, ffmpeg pads. */
    return 1;
}

ssize_t decode_read_audio(DecodeSource *d, int16_t *buf, size_t max_samples)
{
    if (!d || d->fd_a < 0 || d->ended_a) return 0;
    size_t bytes = max_samples * sizeof(int16_t);
    ssize_t got = read_full(d->fd_a, buf, bytes);
    if (got < 0) return -1;
    if (got == 0) { d->ended_a = true; return 0; }
    return got / (ssize_t)sizeof(int16_t);
}
