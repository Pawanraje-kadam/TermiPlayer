/* tui.h — the interactive library interface.
 *
 * Information architecture (borrowed from cmus and termusic, because they got
 * the three-pane model right): a library list on the left, the queue/now-playing
 * on the right, a status and key bar along the bottom. Everything is reachable
 * in one keystroke and discoverable from `?`.
 *
 * Playback is embedded: the video raster is drawn into the right-hand pane at
 * whatever quality the throughput controller allows, and the text UI is drawn
 * around it in the same frame. That is only possible because the screen layer
 * and the graphics backends both write into one TtyWriter.
 */
#ifndef TP_TUI_H
#define TP_TUI_H

#include <stdbool.h>

#include "library.h"
#include "session.h"

typedef struct {
    const char *data_dir;
    const char *start_path;     /* play this immediately          */
    const char *force_backend;
    int         target_fps;
    bool        no_audio;
    bool        no_video;
    bool        stats;
} TuiOpts;

int tui_run(const TuiOpts *o);

#endif /* TP_TUI_H */
