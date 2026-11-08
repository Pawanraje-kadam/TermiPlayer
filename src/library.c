#include "library.h"

#include <limits.h>

#include "decode.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 200000
#define MAX_FOLDERS 256
#define PROBE_WORKER_BUDGET 64      /* probe at most this many new files per scan pass */

struct Library {
    char  *dir;
    char  *index_path;
    char  *state_path;
    char  *ffmpeg;

    Entry *entries;
    int    n, cap;

    char  *folders[MAX_FOLDERS];
    int    nfolders;

    bool   dirty;
    int64_t last_save_ns;
};

/* ------------------------------------------------------------------ */
/* small filesystem helpers                                           */
/* ------------------------------------------------------------------ */

static char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *p = xmalloc(la + lb + 2);
    memcpy(p, a, la);
    if (la && a[la - 1] != '/') p[la++] = '/';
    memcpy(p + la, b, lb + 1);
    return p;
}

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) { free(tmp); return false; }
        *p = '/';
    }
    bool ok = (mkdir(tmp, 0700) == 0 || errno == EEXIST);
    free(tmp);
    return ok;
}

/* Write a file atomically: temp in the same directory, then rename. A crash
 * mid-write leaves the old file intact rather than a truncated one. */
static bool write_atomic(const char *path, const char *data, size_t len)
{
    char *tmp = xmalloc(strlen(path) + 16);
    sprintf(tmp, "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return false; }
    bool ok = fwrite(data, 1, len, f) == len;
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) unlink(tmp);
    free(tmp);
    return ok;
}

/* Escape a string for the line format: backslash, newline, tab. */
static void append_escaped(Buf *b, const char *s)
{
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
        case '\\': buf_puts(b, "\\\\"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:   buf_putc(b, *p); break;
        }
    }
}

/* Read one escaped field up to `sep`, into a malloc'd string. */
static char *read_field(const char **pp, char sep)
{
    const char *p = *pp;
    Buf b; buf_init(&b);
    while (*p && *p != sep) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': buf_putc(&b, '\n'); break;
            case 't': buf_putc(&b, '\t'); break;
            case '\\': buf_putc(&b, '\\'); break;
            default: buf_putc(&b, *p); break;
            }
            p++;
            continue;
        }
        buf_putc(&b, *p++);
    }
    if (*p == sep) p++;
    buf_putc(&b, '\0');
    *pp = p;
    return b.p ? b.p : xstrdup("");
}

/* ------------------------------------------------------------------ */
/* entries                                                            */
/* ------------------------------------------------------------------ */

/* One file must have exactly one identity in the library. Without this, the
 * same movie played as "movie.mkv" and "/home/me/movie.mkv" became two entries
 * with two independent resume points, and "continue" showed the wrong one. */
static char *canon_path(const char *path)
{
    if (!path || !*path) return xstrdup(path ? path : "");
    char buf[4096];
    if (realpath(path, buf)) return xstrdup(buf);
    /* A file that does not exist yet cannot be canonicalised; fall back to the
     * path as given so callers still get a usable key. */
    return xstrdup(path);
}

static int entry_find(const Library *l, const char *path)
{
    /* linear is fine to ~10k and keeps this file simple; the ROADMAP notes the
     * hash-map upgrade for larger libraries. */
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->entries[i].path, path) == 0) return i;
    return -1;
}

static void entry_clear(Entry *e)
{
    free(e->path);
    free(e->title);
    free(e->artist);
    memset(e, 0, sizeof *e);
}

static Entry *entry_new(Library *l, const char *path)
{
    if (l->n >= MAX_ENTRIES) return NULL;
    if (l->n >= l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        l->entries = xrealloc(l->entries, (size_t)cap * sizeof *l->entries);
        l->cap = cap;
    }
    Entry *e = &l->entries[l->n++];
    memset(e, 0, sizeof *e);
    e->path = xstrdup(path);
    e->kind = media_ext_kind(path);

    char *pretty = media_pretty_title(path);
    media_split_artist(pretty, &e->artist, &e->title);
    free(pretty);

    struct stat st;
    if (stat(path, &st) == 0) {
        e->mtime = (int64_t)st.st_mtime;
        e->size = (int64_t)st.st_size;
    }
    return e;
}

/* ------------------------------------------------------------------ */
/* persistence                                                        */
/* ------------------------------------------------------------------ */

/* Index format, one line per entry:
 *   I <path> \t kind \t duration \t mtime \t size
 * State format, one line per entry that has any:
 *   S <path> \t position \t watched \t last_seen
 * Folders:
 *   F <dir>
 * Plain text, tab-separated, path escaped. Easy to inspect and repair. */
static void save_index(Library *l)
{
    Buf b; buf_init(&b);
    buf_puts(&b, "# termiplayer index v1\n");
    for (int i = 0; i < l->nfolders; i++) {
        buf_puts(&b, "F\t");
        append_escaped(&b, l->folders[i]);
        buf_putc(&b, '\n');
    }
    for (int i = 0; i < l->n; i++) {
        Entry *e = &l->entries[i];
        if (e->duration <= 0 && e->mtime == 0) continue;   /* nothing worth caching */
        buf_puts(&b, "I\t");
        append_escaped(&b, e->path);
        buf_printf(&b, "\t%d\t%.3f\t%lld\t%lld\n", (int)e->kind, e->duration,
                   (long long)e->mtime, (long long)e->size);
    }
    write_atomic(l->index_path, b.p ? b.p : "", b.len);
    buf_free(&b);
}

static void save_state(Library *l)
{
    Buf b; buf_init(&b);
    buf_puts(&b, "# termiplayer state v1\n");
    for (int i = 0; i < l->n; i++) {
        Entry *e = &l->entries[i];
        if (e->position <= 0.0 && e->watched <= 0.0 && e->last_seen == 0) continue;
        buf_puts(&b, "S\t");
        append_escaped(&b, e->path);
        buf_printf(&b, "\t%.3f\t%.4f\t%lld\n", e->position, e->watched,
                   (long long)e->last_seen);
    }
    write_atomic(l->state_path, b.p ? b.p : "", b.len);
    buf_free(&b);
}

static void load(Library *l)
{
    /* Pass 1: folders + durations. Pass 2: playback state. Two passes because a
     * state line may appear before the index line for the same path. */
    for (int pass = 0; pass < 2; pass++) {
        const char *path = pass == 0 ? l->index_path : l->state_path;
        FILE *f = fopen(path, "rb");
        if (!f) continue;

        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, f)) > 0) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';
            if (line[0] == '#' || len == 0) continue;

            const char *p = line;
            char tag = *p++;
            if (*p == '\t') p++;

            if (pass == 0 && tag == 'F') {
                char *dir = read_field(&p, '\n');
                if (l->nfolders < MAX_FOLDERS && *dir) l->folders[l->nfolders++] = dir;
                else free(dir);
                continue;
            }
            if (pass == 0 && tag == 'I') {
                char *epath = read_field(&p, '\t');
                int kind = atoi(p);
                while (*p && *p != '\t') p++;
                if (*p) p++;
                double dur = atof(p);
                while (*p && *p != '\t') p++;
                if (*p) p++;
                long long mt = atoll(p);
                while (*p && *p != '\t') p++;
                if (*p) p++;
                long long sz = atoll(p);

                char *cp = canon_path(epath);
                if (entry_find(l, cp) < 0) {
                    Entry *e = entry_new(l, cp);
                    if (e) {
                        if (kind == MK_VIDEO || kind == MK_AUDIO) e->kind = (MediaKind)kind;
                        e->duration = dur;
                        e->mtime = mt;
                        e->size = sz;
                    }
                }
                free(cp);
                free(epath);
                continue;
            }
            if (pass == 1 && tag == 'S') {
                char *epath = read_field(&p, '\t');
                double pos = atof(p);
                while (*p && *p != '\t') p++;
                if (*p) p++;
                double wat = atof(p);
                while (*p && *p != '\t') p++;
                if (*p) p++;
                long long seen = atoll(p);

                /* Create the entry if the file was played directly (e.g.
                 * "player movie.mkv") without ever being indexed. Dropping
                 * these lines is what silently broke resume for every file
                 * outside a watched folder. */
                Entry *e = lib_touch(l, epath);
                if (e) {
                    e->position = pos;
                    e->watched = wat;
                    e->last_seen = seen;
                }
                free(epath);
                continue;
            }
        }
        free(line);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* open / close                                                       */
/* ------------------------------------------------------------------ */

Library *lib_open(const char *data_dir, const char *ffmpeg)
{
    Library *l = xcalloc(1, sizeof *l);
    l->dir = xstrdup(data_dir);
    mkdir_p(data_dir);
    l->index_path = path_join(data_dir, "index.tsv");
    l->state_path = path_join(data_dir, "state.tsv");
    l->ffmpeg = ffmpeg ? xstrdup(ffmpeg) : NULL;
    load(l);
    return l;
}

void lib_close(Library *l)
{
    if (!l) return;
    if (l->dirty) { save_index(l); save_state(l); }
    for (int i = 0; i < l->n; i++) entry_clear(&l->entries[i]);
    for (int i = 0; i < l->nfolders; i++) free(l->folders[i]);
    free(l->entries);
    free(l->dir);
    free(l->index_path);
    free(l->state_path);
    free(l->ffmpeg);
    free(l);
}

void lib_flush(Library *l)
{
    if (!l || !l->dirty) return;
    save_index(l);
    save_state(l);
    l->dirty = false;
    l->last_save_ns = now_ns();
}

/* ------------------------------------------------------------------ */
/* folders and scanning                                               */
/* ------------------------------------------------------------------ */

bool lib_add_folder(Library *l, const char *dir)
{
    if (l->nfolders >= MAX_FOLDERS || !dir || !*dir) return false;
    for (int i = 0; i < l->nfolders; i++)
        if (strcmp(l->folders[i], dir) == 0) return true;

    char resolved[4096];
    const char *use = dir;
    if (realpath(dir, resolved)) use = resolved;

    if (!is_dir(use)) return false;
    l->folders[l->nfolders++] = xstrdup(use);
    l->dirty = true;
    lib_flush(l);
    return true;
}

int lib_folder_count(const Library *l) { return l->nfolders; }

const char *lib_folder(const Library *l, int i)
{
    if (i < 0 || i >= l->nfolders) return NULL;
    return l->folders[i];
}

int lib_count(const Library *l) { return l->n; }

const Entry *lib_at(const Library *l, int i)
{
    if (i < 0 || i >= l->n) return NULL;
    return &l->entries[i];
}

Entry *lib_find(Library *l, const char *path)
{
    char *cp = canon_path(path);
    int i = entry_find(l, cp);
    free(cp);
    return i < 0 ? NULL : &l->entries[i];
}

/* Directories not worth descending into. */
static bool skip_dir(const char *name)
{
    static const char *const SKIP[] = {
        ".git", ".hg", ".svn", "node_modules", "__pycache__", ".cache",
        ".thumbnails", "lost+found", NULL
    };
    if (name[0] == '.') return true;
    for (int i = 0; SKIP[i]; i++) if (strcmp(name, SKIP[i]) == 0) return true;
    return false;
}

static int scan_dir(Library *l, const char *dir, ScanProgress cb, void *ctx,
                    int found, int depth, int *probed)
{
    if (depth > 12) return found;

    DIR *d = opendir(dir);
    if (!d) return found;

    struct dirent *de;
    /* collect names first so we can sort them; directory order is arbitrary and
     * an unsorted library view is disorienting */
    char **names = NULL;
    int nnames = 0, capnames = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' && de->d_name[1] == '\0') continue;
        if (de->d_name[0] == '.' && de->d_name[1] == '.' && !de->d_name[2]) continue;
        if (nnames >= capnames) {
            capnames = capnames ? capnames * 2 : 64;
            names = xrealloc(names, (size_t)capnames * sizeof *names);
        }
        names[nnames++] = xstrdup(de->d_name);
    }
    closedir(d);

    /* simple insertion sort: directories are rarely huge at one level */
    for (int i = 1; i < nnames; i++) {
        char *key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) { names[j + 1] = names[j]; j--; }
        names[j + 1] = key;
    }

    for (int i = 0; i < nnames; i++) {
        char *full = path_join(dir, names[i]);
        struct stat st;

        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!skip_dir(names[i]))
                found = scan_dir(l, full, cb, ctx, found, depth + 1, probed);
            free(full);
            continue;
        }
        if (!media_ext_known(full)) { free(full); continue; }

        Entry *e = lib_find(l, full);
        if (!e) {
            e = entry_new(l, full);
            l->dirty = true;
        } else {
            /* refresh the cheap stat, keep the cached duration */
            e->mtime = (int64_t)st.st_mtime;
            e->size = (int64_t)st.st_size;
            e->kind = media_ext_kind(full);
        }
        found++;

        if (cb && (found % 8) == 0) cb(ctx, found, full, false);

        /* probe duration for anything we do not have yet, but bound the work
         * per pass so a first scan of a huge library stays responsive */
        if (e && e->duration <= 0 && l->ffmpeg && *probed < PROBE_WORKER_BUDGET) {
            (*probed)++;
            if (cb) cb(ctx, found, full, true);
            char err[256];
            MediaInfo mi;
            DecodeSource *ds = decode_open(full, true, true, &mi, err, sizeof err);
            if (ds) {
                e->duration = mi.duration;
                if (mi.has_video) e->kind = MK_VIDEO;
                else if (mi.has_audio) e->kind = MK_AUDIO;
                decode_close(ds);
                l->dirty = true;
            }
        }
        free(full);
    }

    for (int i = 0; i < nnames; i++) free(names[i]);
    free(names);
    return found;
}

int lib_scan(Library *l, ScanProgress cb, void *ctx)
{
    int found = 0;
    int probed = 0;
    for (int i = 0; i < l->nfolders; i++)
        found = scan_dir(l, l->folders[i], cb, ctx, found, 0, &probed);
    lib_flush(l);
    return found;
}

/* ------------------------------------------------------------------ */
/* play state                                                         */
/* ------------------------------------------------------------------ */

Entry *lib_touch(Library *l, const char *path)
{
    char *cp = canon_path(path);
    Entry *e = lib_find(l, cp);
    if (!e) {
        e = entry_new(l, cp);
        l->dirty = true;
    }
    free(cp);
    return e;
}

void lib_set_position(Library *l, Entry *e, double seconds, double duration)
{
    if (!l || !e) return;
    if (seconds < 0) seconds = 0;

    double dur = duration > 0 ? duration : e->duration;
    e->position = seconds;
    e->last_seen = (int64_t)time(NULL);
    if (dur > 0) {
        e->watched = clampd(seconds / dur, 0.0, 1.0);
        if (e->duration <= 0) e->duration = dur;
    }
    l->dirty = true;

    /* Debounce: the playback loop calls this often. Persist at most every 3 s. */
    if (now_ns() - l->last_save_ns > 3000000000LL) {
        save_state(l);
        l->last_save_ns = now_ns();
        l->dirty = false;
    }
}

void lib_set_finished(Library *l, Entry *e)
{
    if (!l || !e) return;
    e->watched = 1.0;
    e->position = 0.0;
    e->last_seen = (int64_t)time(NULL);
    l->dirty = true;
    save_state(l);
}

/* ------------------------------------------------------------------ */
/* views                                                              */
/* ------------------------------------------------------------------ */

int *lib_continue_watching(const Library *l, int *count)
{
    int *idx = xmalloc((size_t)(l->n + 1) * sizeof *idx);
    int n = 0;
    for (int i = 0; i < l->n; i++) {
        const Entry *e = &l->entries[i];
        if (e->position > 5.0 && e->watched < 0.97) idx[n++] = i;
    }
    /* most recent first */
    for (int i = 1; i < n; i++) {
        int key = idx[i], j = i - 1;
        while (j >= 0 && l->entries[idx[j]].last_seen < l->entries[key].last_seen) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }
    *count = n;
    return idx;
}

static bool match_all(const Entry *e, char **terms, int nterms)
{
    for (int t = 0; t < nterms; t++) {
        /* case-insensitive substring across title, artist and path */
        const char *term = terms[t];
        bool hit = false;
        const char *fields[3] = { e->title, e->artist, e->path };
        for (int f = 0; f < 3 && !hit; f++) {
            if (!fields[f]) continue;
            const char *h = fields[f];
            size_t tl = strlen(term);
            for (const char *p = h; *p; p++) {
                size_t k = 0;
                while (k < tl && p[k]) {
                    unsigned char a = (unsigned char)p[k], b = (unsigned char)term[k];
                    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
                    if (a != b) break;
                    k++;
                }
                if (k == tl) { hit = true; break; }
            }
        }
        if (!hit) return false;
    }
    return true;
}

int *lib_filter(const Library *l, const char *filter, int *count)
{
    /* split the filter into terms */
    char *copy = xstrdup(filter ? filter : "");
    char *terms[16];
    int nterms = 0;
    char *save = NULL;
    for (char *tok = strtok_r(copy, " \t", &save); tok && nterms < 16;
         tok = strtok_r(NULL, " \t", &save))
        terms[nterms++] = tok;

    int *idx = xmalloc((size_t)(l->n + 1) * sizeof *idx);
    int n = 0;
    for (int i = 0; i < l->n; i++) {
        if (nterms && !match_all(&l->entries[i], terms, nterms)) continue;
        idx[n++] = i;
    }

    /* sort by title, then path (insertion sort: dune-scale libraries) */
    for (int i = 1; i < n; i++) {
        int key = idx[i], j = i - 1;
        const char *kt = l->entries[key].title ? l->entries[key].title : "";
        while (j >= 0) {
            const char *jt = l->entries[idx[j]].title ? l->entries[idx[j]].title : "";
            int c = strcasecmp(jt, kt);
            if (c < 0) break;
            if (c == 0 && strcmp(l->entries[idx[j]].path, l->entries[key].path) <= 0) break;
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = key;
    }

    free(copy);
    *count = n;
    return idx;
}

double lib_total_watched(const Library *l)
{
    double t = 0;
    for (int i = 0; i < l->n; i++)
        if (l->entries[i].watched > 0) t += l->entries[i].duration * l->entries[i].watched;
    return t;
}

void lib_stats(const Library *l, int *n_video, int *n_audio, double *hours)
{
    int v = 0, a = 0;
    double secs = 0;
    for (int i = 0; i < l->n; i++) {
        if (l->entries[i].kind == MK_VIDEO) v++;
        else if (l->entries[i].kind == MK_AUDIO) a++;
        secs += l->entries[i].duration;
    }
    if (n_video) *n_video = v;
    if (n_audio) *n_audio = a;
    if (hours) *hours = secs / 3600.0;
}
