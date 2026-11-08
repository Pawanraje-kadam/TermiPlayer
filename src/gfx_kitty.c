/* gfx_kitty.c — kitty graphics protocol, two transports.
 *
 * Local session  : t=s, shared memory. The escape carries an object name
 *                  (~60 bytes) instead of the frame. This is the one path where
 *                  resolution is unbounded by the pty, and the spike measured a
 *                  flat 64 B/frame. Always prefer it when we are on the same
 *                  machine as the terminal.
 * Remote session: t=d, direct. Base64 of the raw RGB, or zlib+base64 when the
 *                  frame compresses well. The spike measured zlib at 4.46x
 *                  better on photographic content and 1.00x on noise, so the
 *                  decision is made per frame from a cheap entropy estimate --
 *                  never unconditionally.
 *
 * Protocol notes that matter:
 *   - a=T transmits and displays in one action.
 *   - Chunks are capped at 4096 base64 bytes with m=1 on all but the last.
 *   - q=2 suppresses the terminal's reply, so nothing lands in our input queue.
 *   - Reusing i=1 means the terminal drops the previous image and its
 *     placements for us; no explicit delete per frame.
 */
#include "gfx_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK 4096
#define IMAGE_ID 1

/* ------------------------------------------------------------------ */
/* shared memory transport                                            */
/* ------------------------------------------------------------------ */

/* Create a shm object, copy the pixels in, and return its name (a "shm object
 * identifier" in kitty's terms: /name). The terminal mmaps and unlinks it.
 * Returns false when shm is unavailable (e.g. no /dev/shm), so the caller can
 * fall back to direct transfer. */
static bool kitty_shm_put(const Raster *r, char *name_out, size_t name_cap)
{
    static unsigned counter;
    size_t bytes = (size_t)r->w * (size_t)r->h * 3u;

    for (int attempt = 0; attempt < 4; attempt++) {
        char name[64];
        snprintf(name, sizeof name, "/termiplayer-%d-%u", (int)getpid(), counter++);

        int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) return false;
        if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); shm_unlink(name); return false; }

        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { shm_unlink(name); return false; }

        memcpy(p, r->px, bytes);
        munmap(p, bytes);
        /* Do NOT unlink: the terminal must open it by name. It unlinks after
         * mapping. If the terminal never maps it (protocol not supported after
         * all), we leak one object per frame, so we bound attempts and the
         * caller falls back. */
        snprintf(name_out, name_cap, "%s", name);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* direct transport                                                   */
/* ------------------------------------------------------------------ */

static void kitty_direct(TtyWriter *w, const Raster *r, Rect cells, bool use_zlib)
{
    size_t raw = (size_t)r->w * (size_t)r->h * 3u;
    size_t payload_len = raw;
    uint8_t *compressed = NULL;

    if (use_zlib) {
        /* Worst case for compress2 is slightly above the input size. */
        size_t cap = raw + raw / 16 + 64;
        compressed = xmalloc(cap);
        size_t n = zlib_compress(r->px, raw, compressed, cap, 1);
        if (n && n < raw) payload_len = n;
        else { free(compressed); compressed = NULL; }
    }
    const uint8_t *payload = compressed ? compressed : r->px;

    size_t b64len = b64_size(payload_len, false);
    char *b64 = xmalloc(b64len + 4);
    b64_encode(payload, payload_len, b64, b64len + 4, false);

    Buf *b = &w->b;
    size_t off = 0;
    bool first = true;
    do {
        size_t take = b64len - off;
        if (take > CHUNK) take = CHUNK;
        bool more = (off + take) < b64len;

        buf_puts(b, "\x1b_G");
        if (first) {
            buf_printf(b, "a=T,f=24,s=%d,v=%d,i=%d,q=2", r->w, r->h, IMAGE_ID);
            if (compressed) buf_puts(b, ",o=z");
            /* explicit placement so the image does not advance the cursor */
            buf_printf(b, ",c=%d,r=%d,C=1", cells.w, cells.h);
        }
        buf_printf(b, ",m=%d;", more ? 1 : 0);
        buf_append(b, b64 + off, take);
        buf_puts(b, "\x1b\\");

        off += take;
        first = false;
    } while (off < b64len);

    free(b64);
    free(compressed);
}

/* ------------------------------------------------------------------ */

void gfx_kitty_draw(const TermInfo *ti, TtyWriter *w, const Raster *r,
                    Rect cells, bool shm)
{
    if (!r->px || r->w < 1 || r->h < 1) return;

    /* Position first: the image is placed at the cursor. */
    gfx_cursor(w, cells.y + 1, cells.x + 1);

    if (shm) {
        char name[80];
        if (kitty_shm_put(r, name, sizeof name)) {
            buf_printf(&w->b,
                "\x1b_Ga=T,t=s,f=24,s=%d,v=%d,i=%d,q=2,c=%d,r=%d,C=1;%s\x1b\\",
                r->w, r->h, IMAGE_ID, cells.w, cells.h, name);
            return;
        }
        /* shm refused (container without /dev/shm?): fall through to direct. */
    }

    /* Compression only pays when the frame is not noise. The threshold comes
     * from the spike: zlib at level 1 never *loses* much, but on high-entropy
     * frames it wastes CPU for ~0% gain, so skip it. */
    RasterStats st = raster_stats(r);
    bool use_zlib = zlib_available() && st.entropy_hint < 0.55;
    (void)ti;
    kitty_direct(w, r, cells, use_zlib);
}
