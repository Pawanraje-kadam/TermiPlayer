/* library.h — the collection, and the memory of it.
 *
 * This is the half of the product nobody else in this niche has built: not
 * "render pixels", but "treat these files as a collection and remember what I
 * was doing".
 *
 * Two responsibilities, deliberately in one place because they share a key (the
 * path):
 *
 *   1. INDEX    — walk watched folders, find playable files, probe duration.
 *                 Cached so a restart does not re-probe 10,000 files.
 *   2. STATE    — per-file playback position, watched status, last seen; plus
 *                 the queue and playlists.
 *
 * Storage is a line-oriented text file, written atomically (temp + rename), not
 * SQLite: there is no sqlite dev package in this environment, and a file that a
 * human can read and repair is a better failure mode than a corrupt database for
 * a tool this size. See docs/ROADMAP.md for when to migrate.
 */
#ifndef TP_LIBRARY_H
#define TP_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "media.h"

typedef struct {
    char      *path;
    char      *title;
    char      *artist;       /* NULL when not "Artist - Title" */
    MediaKind  kind;
    double     duration;     /* 0 = unknown */
    double     position;     /* resume point, 0 = never played */
    double     watched;      /* 0..1, 1 = finished */
    int64_t    last_seen;    /* unix seconds, 0 = never */
    int64_t    mtime;
    int64_t    size;
} Entry;

typedef struct Library Library;

/* Open (or create) the library rooted at `data_dir` (created if needed).
 * `ffmpeg` is the binary used to probe durations; NULL disables probing, in
 * which case entries get duration 0 and everything still works. */
Library *lib_open(const char *data_dir, const char *ffmpeg);
void     lib_close(Library *l);

/* Add a directory to the watch list and remember it. */
bool lib_add_folder(Library *l, const char *dir);
int  lib_folder_count(const Library *l);
const char *lib_folder(const Library *l, int i);

/* Rescan the watch list. `progress` is called with (files_seen, current_path)
 * so the UI can show what it is doing. Reuses cached durations when a file's
 * (mtime, size) is unchanged. Returns the number of files indexed. */
typedef void (*ScanProgress)(void *ctx, int found, const char *path, bool probing);
int lib_scan(Library *l, ScanProgress progress, void *ctx);

/* Number of indexed entries. */
int lib_count(const Library *l);
const Entry *lib_at(const Library *l, int i);
/* Look up by path; NULL if unknown. */
Entry *lib_find(Library *l, const char *path);

/* Ensure `path` is in the index even if it is outside the watched folders
 * (used when a file is played explicitly). Returns the entry. */
Entry *lib_touch(Library *l, const char *path);

/* Update and persist the resume point. Writes to disk on a short debounce so a
 * 30 fps playback loop does not fsync 30 times a second. */
void lib_set_position(Library *l, Entry *e, double seconds, double duration);
/* Mark finished (watched = 1, position = 0). */
void lib_set_finished(Library *l, Entry *e);
/* Force everything to disk now. Called on quit and on SIGINT. */
void lib_flush(Library *l);

/* ---- selection views the UI needs ---- */

/* Indices sorted for "continue watching": partially watched, most recent first.
 * Returns a malloc'd array; caller frees. */
int *lib_continue_watching(const Library *l, int *count);

/* Case-insensitive substring filter over title/artist/path. `filter` may be a
 * space-separated list of terms, all of which must match (the way a user types
 * "dune 2021"). Returns malloc'd indices, sorted by title. */
int *lib_filter(const Library *l, const char *filter, int *count);

/* Total duration of everything watched, for the header. */
double lib_total_watched(const Library *l);

/* Human-readable summary for the UI footer. */
void lib_stats(const Library *l, int *n_video, int *n_audio, double *hours);

#endif /* TP_LIBRARY_H */
