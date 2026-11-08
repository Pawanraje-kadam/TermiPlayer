#include "media.h"
#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *ext; MediaKind kind; } ExtEnt;

static const ExtEnt EXT[] = {
    /* video */
    { "mkv", MK_VIDEO }, { "mp4", MK_VIDEO }, { "m4v", MK_VIDEO },
    { "avi", MK_VIDEO }, { "mov", MK_VIDEO }, { "webm", MK_VIDEO },
    { "flv", MK_VIDEO }, { "wmv", MK_VIDEO }, { "mpg", MK_VIDEO },
    { "mpeg", MK_VIDEO }, { "ts", MK_VIDEO }, { "m2ts", MK_VIDEO },
    { "ogv", MK_VIDEO }, { "3gp", MK_VIDEO }, { "vob", MK_VIDEO },
    { "mts", MK_VIDEO }, { "divx", MK_VIDEO },
    /* audio */
    { "mp3", MK_AUDIO }, { "flac", MK_AUDIO }, { "opus", MK_AUDIO },
    { "ogg", MK_AUDIO }, { "oga", MK_AUDIO }, { "m4a", MK_AUDIO },
    { "aac", MK_AUDIO }, { "wav", MK_AUDIO }, { "wv", MK_AUDIO },
    { "ape", MK_AUDIO }, { "alac", MK_AUDIO }, { "aiff", MK_AUDIO },
    { "mka", MK_AUDIO }, { "wma", MK_AUDIO }, { "dsf", MK_AUDIO },
    { "mpc", MK_AUDIO }, { "spx", MK_AUDIO },
};

static const char *ext_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    if (!dot || (slash && dot < slash)) return NULL;
    return dot + 1;
}

static bool ext_is(const char *path, const char *want)
{
    const char *e = ext_of(path);
    if (!e) return false;
    while (*e && *want) {
        if (tolower((unsigned char)*e) != tolower((unsigned char)*want)) return false;
        e++; want++;
    }
    return *e == '\0' && *want == '\0';
}

bool media_ext_known(const char *path)
{
    return media_ext_kind(path) != MK_UNKNOWN;
}

MediaKind media_ext_kind(const char *path)
{
    const char *e = ext_of(path);
    if (!e) return MK_UNKNOWN;
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++)
        if (ext_is(path, EXT[i].ext)) return EXT[i].kind;
    return MK_UNKNOWN;
}

/* Tokens that carry no information a human wants to read. Matched as whole
 * words, case-insensitively, so "web-dl" and "WEB.DL" both go. */
static const char *const NOISE_EXACT[] = {
    "1080p", "720p", "2160p", "480p", "576p", "4k", "8k", "uhd", "hd", "sd",
    "x264", "x265", "h264", "h265", "hevc", "avc", "av1", "xvid", "divx",
    "webrip", "web", "dl", "webdl", "web-dl", "bluray", "blu-ray", "brrip",
    "bdrip", "dvdrip", "dvd", "hdtv", "pdtv", "hdrip", "remux", "proper",
    "repack", "internal", "limited", "extended", "unrated", "remastered",
    "directors", "cut", "aac", "ac3", "eac3", "dts", "dtshd", "truehd",
    "atmos", "flac", "mp3", "opus", "10bit", "8bit", "hdr", "hdr10", "dv",
    "dolby", "vision", "sdr", "yify", "yts", "rarbg", "ettv", "eztv",
    "multi", "dual", "audio", "subs", "subbed", "dubbed", "amzn", "nf",
    "dsnp", "hmax", "atvp", "max", "part1", "part2",
    NULL
};

/* Prefixes that make a token noise regardless of trailing digits, e.g. "x265"
 * variants like "x265-10bit" or "h264-crf20". */
static const char *const NOISE_PREFIX[] = {
    "x264", "x265", "h264", "h265", "hevc", "webrip", "web-dl", "bluray",
    NULL
};

static bool token_is_noise(const char *tok)
{
    char low[64];
    size_t n = 0;
    for (; tok[n] && n + 1 < sizeof low; n++) {
        char c = tok[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        low[n] = c;
    }
    low[n] = '\0';
    if (!n) return true;

    for (int i = 0; NOISE_EXACT[i]; i++)
        if (!strcmp(low, NOISE_EXACT[i])) return true;

    for (int i = 0; NOISE_PREFIX[i]; i++) {
        size_t pl = strlen(NOISE_PREFIX[i]);
        if (!strncmp(low, NOISE_PREFIX[i], pl)) return true;
    }

    /* a bare 3-4 letter group tag after a dash is stripped before we get here,
     * but lone quality words like "rip" also carry nothing */
    if (!strcmp(low, "rip") || !strcmp(low, "sub") || !strcmp(low, "eng") ||
        !strcmp(low, "ita") || !strcmp(low, "jpn") || !strcmp(low, "fra") ||
        !strcmp(low, "ger") || !strcmp(low, "spa"))
        return true;

    /* a token that is only digits AND 1-2 long is a track number, not a year */
    if (n <= 2) {
        bool alldig = true;
        for (size_t i = 0; i < n; i++) if (low[i] < '0' || low[i] > '9') { alldig = false; break; }
        if (alldig) return true;
    }
    return false;
}

char *media_pretty_title(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    char *s = xstrdup(base);

    /* drop the extension */
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = '\0';

    /* Music convention: "Artist - Title" (or "01 - Artist - Title"): the part
     * after the LAST " - " is the title, and it is already human-readable.
     * Do this before any token stripping, which would mangle real words. */
    char *lastsep = NULL;
    for (char *p = strstr(s, " - "); p; p = strstr(p + 1, " - "))
        lastsep = p;
    if (lastsep && *(lastsep + 3)) {
        char *title = xstrdup(lastsep + 3);
        free(s);
        s = title;
    } else {
        /* strip a trailing [group] or (tag) */
        for (;;) {
            size_t n = strlen(s);
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
            if (n > 0 && (s[n - 1] == ']' || s[n - 1] == ')')) {
                char open = s[n - 1] == ']' ? '[' : '(';
                char *p = strrchr(s, open);
                if (p && strlen(p) < 40) { *p = '\0'; continue; }
            }
            break;
        }

        /* strip a trailing -GROUP (release group) */
        char *dash = strrchr(s, '-');
        if (dash && dash != s) {
            size_t gl = strlen(dash + 1);
            bool looks_group = (gl >= 2 && gl <= 20);
            for (size_t i = 0; i < gl && looks_group; i++) {
                char c = dash[1 + i];
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '_'))
                    looks_group = false;
            }
            if (looks_group) *dash = '\0';
        }

        /* separators to spaces */
        for (char *p = s; *p; p++)
            if (*p == '.' || *p == '_') *p = ' ';

        /* leading track number */
        char *p = s;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { p++; digits++; }
        if (digits > 0 && digits <= 3) {
            char *q = p;
            while (*q == ' ' || *q == '-') q++;
            if (q > p && *q) memmove(s, q, strlen(q) + 1);
        }

        /* token filter */
        char *out = s;
        const char *r = s;
        bool wrote = false;
        while (*r) {
            while (*r == ' ') r++;
            if (!*r) break;
            const char *w = r;
            while (*r && *r != ' ') r++;
            size_t wl = (size_t)(r - w);

            char tok[128];
            if (wl >= sizeof tok) wl = sizeof tok - 1;
            memcpy(tok, w, wl);
            tok[wl] = '\0';

            if (token_is_noise(tok)) continue;

            if (wrote) *out++ = ' ';
            memcpy(out, tok, wl);
            out += wl;
            wrote = true;
        }
        *out = '\0';
    }

    /* collapse whitespace and trim */
    char *src = s, *dst = s;
    bool space = false;
    for (; *src; src++) {
        if (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') { space = true; continue; }
        if (space && dst != s) *dst++ = ' ';
        space = false;
        *dst++ = *src;
    }
    *dst = '\0';

    /* if the name was all one case, make it readable: "the matrix" -> "The Matrix" */
    bool has_lower = false, has_upper = false;
    for (char *p = s; *p; p++) {
        if (*p >= 'a' && *p <= 'z') has_lower = true;
        if (*p >= 'A' && *p <= 'Z') has_upper = true;
    }
    if (has_lower && !has_upper) {
        bool start = true;
        for (char *p = s; *p; p++) {
            if (*p == ' ') { start = true; continue; }
            if (start && *p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            start = false;
        }
    }

    if (s[0] == '\0') {
        free(s);
        return xstrdup(base);
    }
    return s;
}

void media_split_artist(const char *pretty, char **artist, char **title)
{
    *artist = NULL;
    *title = NULL;
    if (!pretty) return;

    const char *sep = strstr(pretty, " - ");
    if (!sep) {
        *title = xstrdup(pretty);
        return;
    }
    *artist = xmalloc((size_t)(sep - pretty) + 1);
    memcpy(*artist, pretty, (size_t)(sep - pretty));
    (*artist)[sep - pretty] = '\0';
    *title = xstrdup(sep + 3);
}
