/* session.h — the playback loop.
 *
 * One loop, three modes, because they share 90% of their code:
 *   testsrc  : synthetic frames, no decode, no audio
 *   video    : decode video + audio, audio master clock
 *   audio    : decode audio only, no raster work at all
 *
 * The loop is deliberately not a full TUI. M3 adds the library views on top;
 * this loop is what those views call into, so it must stay usable headless
 * (tests drive it with TERMIPLAYER_AUDIO_FD and a pty).
 */
#ifndef TP_SESSION_H
#define TP_SESSION_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *path;          /* NULL for testsrc            */
    bool  testsrc;
    int   testsrc_seconds;

    const char *force_backend; /* "auto" or a backend name    */
    int   target_fps;
    int   force_w, force_h;    /* --size                      */

    double start_at;           /* seconds to seek to first    */
    bool  no_audio;            /* decode video only           */
    bool  no_video;            /* audio only                  */
    bool  stats;               /* print a stats line          */
    bool  quiet;               /* no HUD at all               */
    bool  loop;                /* restart at end              */
    double duration_limit;     /* stop after N seconds, 0=off */
    bool   no_resume;          /* ignore a saved resume point */
} SessionOpts;

/* Returns process exit code. */
int session_run(const SessionOpts *o);

#endif /* TP_SESSION_H */
