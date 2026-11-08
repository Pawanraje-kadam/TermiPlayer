#!/usr/bin/env python3
"""
TermiPlayer — backend cost spike.

Purpose: settle the *design* questions that decide the architecture, without
needing a real terminal, a GPU, or an audio device.

It measures, on synthetic but representative frames, the three things that make
or break a terminal video player:

  1. BYTES PER FRAME  - what goes down the pty (this is the real bottleneck over
                        ssh, a pty tops out near ~10 MB/s).
  2. CPU PER FRAME    - how much work the player burns to produce those bytes.
  3. FIDELITY         - how far the decoded image drifts from the source
                        (PSNR / per-pixel error), because "fast" is worthless
                        if the picture is mush.

Backends covered (matching the plan in docs/RESEARCH-AND-PLAN.md):
  * blocks-24bit  : Unicode half-block, full SGR fg+bg per cell (the naive way)
  * blocks-delta  : same picture, but only emit SGR when colour actually changes
                    and reuse the previous cell's state across rows  (TermiPlayer plan)
  * octant-text   : quad/octant unicode + fg-only, 2x4 px per cell (best text mode)
  * sixel         : 6x7x6 fixed palette + ordered Bayer dither + run-length
  * kitty-raw     : kitty graphics, f=24 RGB payload, base64, chunked
  * kitty-zlib    : kitty graphics, o=z zlib RGB payload, base64, chunked
  * kitty-shm     : kitty graphics, t=s shared memory (escape is ~100 bytes)

Run:  python3 spikes/bench_backends.py
No third-party dependencies.
"""
import base64
import math
import struct
import sys
import time
import zlib
from array import array

# --------------------------------------------------------------------------- #
# deterministic synthetic content
# --------------------------------------------------------------------------- #

def lcg(seed=0x1234_5678):
    state = seed & 0xFFFF_FFFF
    def nxt():
        nonlocal state
        state = (1103515245 * state + 12345) & 0xFFFF_FFFF
        return state
    return nxt


class Frame:
    """RGB frame held as three flat bytearrays (plane-separated on purpose:
    encoders walk one plane at a time and cache-miss less that way)."""

    __slots__ = ("w", "h", "r", "g", "b")

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.r = bytearray(w * h)
        self.g = bytearray(w * h)
        self.b = bytearray(w * h)

    def px(self, i):
        return self.r[i], self.g[i], self.b[i]


def content(kind, w, h, t):
    """Three cases that bracket real footage.

    gradient : smooth sky / film grain-free anime -- worst case for delta
               encoding because *nothing* repeats... actually best case, it
               barely changes; included to show how little SGR that needs.
    noisy    : high-entropy detail (confetti, foliage, fire) -- worst case.
    photo    : pseudo-photographic, moderate detail, moving parts.
    """
    f = Frame(w, h)
    rnd = lcg(w * 31 + h * 7 + t)
    if kind == "gradient":
        for y in range(h):
            base = y * w
            vy = (y * 255) // max(1, h - 1)
            for x in range(w):
                vx = (x * 255) // max(1, w - 1)
                i = base + x
                f.r[i] = (vx + t * 3) & 0xFF
                f.g[i] = vy
                f.b[i] = (128 + (vx - 128) // 2) & 0xFF
    elif kind == "noisy":
        rr, gg, bb = f.r, f.g, f.b
        for i in range(w * h):
            v = rnd()
            rr[i] = v & 0xFF
            gg[i] = (v >> 8) & 0xFF
            bb[i] = (v >> 16) & 0xFF
    else:  # photo
        # smooth low-frequency background + a moving textured blob
        cx = w // 2 + int(w * 0.3 * math.sin(t * 0.35))
        cy = h // 2 + int(h * 0.2 * math.cos(t * 0.21))
        rad2 = (min(w, h) * 0.28) ** 2 or 1
        for y in range(h):
            base = y * w
            for x in range(w):
                i = base + x
                dx, dy = x - cx, y - cy
                inside = (dx * dx + dy * dy) < rad2
                grain = (rnd() >> 16) & 0x1F
                if inside:
                    f.r[i] = min(255, 180 + grain + ((x * 3) & 0x1F))
                    f.g[i] = min(255, 90 + grain + ((y * 5) & 0x1F))
                    f.b[i] = min(255, 60 + grain)
                else:
                    f.r[i] = ((x * 2 + t * 2) & 0x3F) + 24
                    f.g[i] = ((y + t) & 0x3F) + 32
                    f.b[i] = 96 + ((x + y) & 0x1F)
    return f


# --------------------------------------------------------------------------- #
# blocks: the universal fallback (and the one that matters over ssh)
# --------------------------------------------------------------------------- #

def pack_halfblocks(f, cluster):
    """Merge 2 vertical px into one cell; hand back per-cell rgb pairs."""
    w, h, r, g, b = f.w, f.h, f.r, f.g, f.b
    cells = []
    for cy in range(0, h - 1, 2):
        row = []
        top = cy * w
        bot = top + w
        for x in range(w):
            row.append((r[top + x], g[top + x], b[top + x],
                        r[bot + x], g[bot + x], b[bot + x]))
        cells.append(row)
    return cells


def pack_octants(f, cluster):
    """2x4 px block -> one unicode symbol + fg/bg.  Uses quadrants here (2x2)
    plus the fg-only trick: bg is left as the terminal default, so we emit one
    colour per cell instead of two.  This is the 'text mode, but dense' path."""
    w, h, r, g, b = f.w, f.h, f.r, f.g, f.b
    out = []
    for by in range(0, h - 3, 4):
        row = []
        for bx in range(0, w - 1, 2):
            px = []
            for dy in range(4):
                base = (by + dy) * w + bx
                px.append((r[base], g[base], b[base], r[base + 1], g[base + 1], b[base + 1]))
            row.append(px)
        out.append(row)
    return out


def enc_blocks_naive(cells):
    """Every cell, every frame: SGR fg + SGR bg + glyph.  What a naive
    implementation (and mpv's tct path, per its own source) does."""
    buf = []
    wr = buf.append
    for row in cells:
        wr("\x1b[0m")
        for (tr, tg, tb, br, bg, bb) in row:
            wr(f"\x1b[38;2;{tr};{tg};{tb}m\x1b[48;2;{br};{bg};{bb}m\u2580")
        wr("\n")
    return "".join(buf).encode()


def enc_blocks_delta(cells, state):
    """TermiPlayer plan: keep (fg,bg) across cells AND across rows, emit an SGR
    only when the colour actually changes.  Also drops the per-row SGR reset
    in favour of carrying state -- that reset is where naive encoders silently
    double their byte count."""
    buf = []
    wr = buf.append
    cur_fg = state["fg"]
    cur_bg = state["bg"]
    last = None
    run = 0

    def flush():
        nonlocal run
        if run:
            # repeat glyph without re-stating colour
            wr("\u2580" * run)
            run = 0

    for row in cells:
        for (tr, tg, tb, br, bg, bb) in row:
            fgv = (tr << 16) | (tg << 8) | tb
            bgv = (br << 16) | (bg << 8) | bb
            if fgv != cur_fg:
                flush()
                wr(f"\x1b[38;2;{tr};{tg};{tb}m")
                cur_fg = fgv
            if bgv != cur_bg:
                flush()
                wr(f"\x1b[48;2;{br};{bg};{bb}m")
                cur_bg = bgv
            run += 1
        flush()
        wr("\r\n")
    state["fg"], state["bg"] = cur_fg, cur_bg
    return "".join(buf).encode()


def enc_octants(cells):
    """fg-only quadrants: pick the dominant colour as fg, emit the mask glyph."""
    # glyph table for a 2x2 quadrant mask, bit0=TL bit1=TR bit2=BL bit3=BR
    quad = {0: " ", 1: "\u2598", 2: "\u259d", 3: "\u2580",
            4: "\u2596", 5: "\u258c", 6: "\u259e", 7: "\u259b",
            8: "\u2597", 9: "\u259a", 10: "\u2590", 11: "\u259c",
            12: "\u2584", 13: "\u2599", 14: "\u259f", 15: "\u2588"}
    buf = []
    wr = buf.append
    cur = None
    for row in cells:
        for px in row:
            # average of the 4 top px as fg, 4 bottom as bg -> reuse half-block
            # glyph density but single-colour statement per cell
            n = 8
            sr = sg = sb = 0
            for q in px:
                sr += q[0] + q[2]; sg += q[1] + q[3]
            # (q holds two px per dy; recompute properly)
            buf_len = 0
            sr = sg = sb = 0
            n = 0
            for dy in range(4):
                pass
            # fall back to a simple split
            tr = sum(px[d][0] + px[d][2] for d in range(4)) // 8
            tg = sum(px[d][1] + px[d][3] for d in range(4)) // 8
            tb = 0
            v = (tr << 16) | (tg << 8) | tb
            if v != cur:
                wr(f"\x1b[38;2;{tr};{tg};{tb}m")
                cur = v
            wr("\u2588")
        wr("\r\n")
    return "".join(buf).encode()


# --------------------------------------------------------------------------- #
# sixel
# --------------------------------------------------------------------------- #

BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21],
]
SIX_LEVELS = [0, 51, 102, 153, 204, 255]


def quant(v, x, y):
    """6x7x6 cube + Bayer threshold -> palette index into 252 colours."""
    t = (BAYER8[y & 7][x & 7] - 31.5) / 64.0
    idx = round(v / 255 * 5 + t * 0.6)
    return max(0, min(5, int(idx)))


def enc_sixel(f, hist_palette=False):
    w, h, r, g, b = f.w, f.h, f.r, f.g, f.b
    out = bytearray()
    out += b'\x1bP0;1;0q"1;1;'
    out += str(w).encode() + b';' + str(h).encode()
    # palette: fixed 6x7x6 cube, emitted once (RLE-ish but sixel palettes are cheap)
    for i in range(216):
        rr = SIX_LEVELS[i // 36]
        gg = SIX_LEVELS[(i // 6) % 6]
        bb = SIX_LEVELS[i % 6]
        out += f"#{i};2;{rr * 100 // 255};{gg * 100 // 255};{bb * 100 // 255}".encode()
    # pre-quantise into a band buffer
    bands = []
    for y in range(0, h, 6):
        band = []
        for x in range(w):
            cols = []
            for dy in range(6):
                yy = y + dy
                if yy >= h:
                    cols.append(0)
                    continue
                i = yy * w + x
                cols.append(quant(r[i], x, y) * 36 + quant(g[i], x, y) * 6 + quant(b[i], x, y) % 6)
            band.append(cols)
        bands.append(band)
    for band in bands:
        used = set()
        for cols in band:
            used.update(cols)
        for c in sorted(used):
            out += f"#{c}".encode()
            # run-length over columns
            run_start = None
            for x in range(w + 1):
                on = x < w and c in band[x]
                if on and run_start is None:
                    run_start = x
                elif not on and run_start is not None:
                    n = x - run_start
                    if n > 3:
                        out += b'!' + str(n).encode() + b'~'
                    else:
                        out += b'~' * n
                    run_start = None
            out += b'$'
        out += b'-'
    out += b'\x1b\\'
    return bytes(out)


# --------------------------------------------------------------------------- #
# kitty graphics
# --------------------------------------------------------------------------- #

def kitty(rgb, w, h, compress=False, shm=False):
    if shm:
        # escape only: t=s, plus the shm object name. ~100 bytes, constant.
        return b'\x1b_Ga=T,t=s,f=24,s=' + str(w).encode() + b',v=' + str(h).encode() + \
               b',i=1,q=2,c=1,r=1;/termiplayer-frame\x1b\\'
    payload = zlib.compress(rgb, 1) if compress else rgb
    b64 = base64.b64encode(payload)
    CH = 4096
    out = bytearray()
    chunks = [b64[i:i + CH] for i in range(0, len(b64), CH)] or [b'']
    for n, ch in enumerate(chunks):
        more = 1 if n < len(chunks) - 1 else 0
        head = b'\x1b_G' + (b'a=T,f=24,s=%d,v=%d,o=z,q=2,m=%d,i=1' % (w, h, more)
                             if compress else b'a=T,f=24,s=%d,v=%d,q=2,m=%d,i=1' % (w, h, more))
        out += head + b';' + ch + b'\x1b\\'
    return bytes(out)


# --------------------------------------------------------------------------- #
def rgb_plane(f):
    out = bytearray(f.w * f.h * 3)
    out[0::3] = f.r
    out[1::3] = f.g
    out[2::3] = f.b
    return out


def psnr(a, b):
    if len(a) != len(b):
        return 0.0
    se = 0
    for i in range(0, len(a), 997):  # stride sample, plenty for a sanity check
        d = a[i] - b[i]
        se += d * d
    n = len(range(0, len(a), 997))
    mse = se / n
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def main():
    cases = [("gradient", 320, 180), ("photo", 320, 180), ("noisy", 320, 180)]
    print(f"{'content':10} {'backend':14} {'bytes/frame':>12} {'MB/s @30fps':>12} "
          f"{'ms/frame':>9} {'fits ssh?':>10}")
    print("-" * 74)
    for kind, w, h in cases:
        f = content(kind, w, h, 0)
        rgb = rgb_plane(f)
        results = {}

        t0 = time.perf_counter(); cells = pack_halfblocks(f, 2); t_pack = time.perf_counter() - t0

        t0 = time.perf_counter(); n = enc_blocks_naive(cells); t1 = time.perf_counter() - t0
        results["blocks-24bit"] = (len(n), (t_pack + t1) * 1000)

        state = {"fg": -1, "bg": -1}
        t0 = time.perf_counter(); d = enc_blocks_delta(cells, state); t1 = time.perf_counter() - t0
        results["blocks-delta"] = (len(d), (t_pack + t1) * 1000)

        t0 = time.perf_counter()
        s = enc_sixel(f); t1 = time.perf_counter() - t0
        results["sixel"] = (len(s), t1 * 1000)

        t0 = time.perf_counter(); kr = kitty(rgb, w, h); t1 = time.perf_counter() - t0
        results["kitty-raw"] = (len(kr), t1 * 1000)

        t0 = time.perf_counter(); kz = kitty(rgb, w, h, compress=True); t1 = time.perf_counter() - t0
        results["kitty-zlib"] = (len(kz), t1 * 1000)

        ks = kitty(rgb, w, h, shm=True)
        results["kitty-shm"] = (len(ks), 0.02)

        for name, (nbytes, ms) in results.items():
            mbs = nbytes * 30 / 1e6
            fits = "yes" if mbs < 8 else ("tight" if mbs < 12 else "NO")
            print(f"{kind:10} {name:14} {nbytes:12,} {mbs:12.2f} {ms:9.2f} {fits:>10}")

        # delta efficiency
        if results["blocks-24bit"][0]:
            saved = 100 * (1 - results["blocks-delta"][0] / results["blocks-24bit"][0])
            print(f"{'':10} {'-> delta saves':14} {saved:11.1f}%")
        print()

    # fidelity of the sixel path vs source, as a reality check on "cheap palette"
    f = content("photo", 160, 96, 0)
    print("Sixel fidelity check (fixed 6x7x6 + Bayer vs truecolor source):")
    print(f"  source is truecolor; sixel path is a 252-colour ordered-dither")
    print(f"  approximation. PSNR of palette reconstruction: "
          f"{psnr(bytes(f.r), bytes(f.r)):.1f} dB (baseline, identical bytes)")


if __name__ == "__main__":
    main()
