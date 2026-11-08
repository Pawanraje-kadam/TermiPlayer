/* gfx_iterm2.c — iTerm2 inline images (OSC 1337).
 *
 * iTerm2 and WezTerm accept base64 image data inline. The protocol wants a file
 * (PNG/GIF/JPEG) normally, but accepts raw pixel data via
 *   File=inline=1;size=<bytes>;width=<cells>;height=<cells>:<base64>
 * with ":" meaning "raw RGB, sizes in pixels". No compression is available
 * (unlike kitty's o=z), so this path re-uploads the full frame every time --
 * measurably the most expensive pixel transport, which is why it sits below
 * kitty in the ladder.
 *
 * The payload is streamed in chunks to avoid building a second copy of the
 * whole frame as base64: at 320x90 that would be a 900 KB transient.
 */
#include "gfx_internal.h"

#include <string.h>

#define IT_CHUNK 8192

void gfx_iterm2_draw(const TermInfo *ti, TtyWriter *w, const Raster *r, Rect cells)
{
    (void)ti;
    if (!r->px || r->w < 1 || r->h < 1) return;

    size_t raw = (size_t)r->w * (size_t)r->h * 3u;
    size_t b64len = b64_size(raw, true);

    gfx_cursor(w, cells.y + 1, cells.x + 1);

    Buf *b = &w->b;
    buf_printf(b,
        "\x1b]1337;File=inline=1;size=%zu;width=%dpx;height=%dpx;preserveAspectRatio=0:",
        raw, r->w, r->h);

    /* chunked encode: fixed temporary of IT_CHUNK input bytes */
    const size_t in_chunk = (IT_CHUNK / 4) * 3;
    char out[IT_CHUNK + 8];
    size_t off = 0;
    while (off < raw) {
        size_t take = raw - off;
        if (take > in_chunk) take = in_chunk;
        size_t n = b64_encode(r->px + off, take, out, sizeof out, true);
        buf_append(b, out, n);
        off += take;
    }
    buf_putc(b, '\a');   /* BEL terminates OSC */
    (void)b64len;
}
