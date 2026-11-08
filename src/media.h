/* media.h — what a file is: type, duration, title, progress.
 *
 * Deliberately flat. A media file has a path, a kind, a duration, a title and a
 * playback position, and that is all the library needs to be useful. Metadata
 * like artist/album is scraped from the filename and container later; it does
 * not belong in the core type.
 */
#ifndef TP_MEDIA_H
#define TP_MEDIA_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MK_UNKNOWN = 0,
    MK_VIDEO,
    MK_AUDIO,
} MediaKind;

/* Extensions we will offer to the player. Kept deliberately broad: ffmpeg
 * decides what actually decodes, this only decides what appears in the library. */
bool media_ext_known(const char *path);
MediaKind media_ext_kind(const char *path);

/* "Artist - Title" -> title; "film.2021.1080p.mkv" -> "film 2021". Strips the
 * extension, replaces separators, drops a leading track number. Returns a
 * malloc'd string. */
char *media_pretty_title(const char *path);

/* Split "Artist - Title" into artist and title when the pattern is present.
 * Both are malloc'd; either may be NULL when not present. */
void media_split_artist(const char *pretty, char **artist, char **title);

#endif /* TP_MEDIA_H */
