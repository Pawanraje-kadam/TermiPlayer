#!/usr/bin/env python3
"""
TermiPlayer — spike #2: the numbers that decide the architecture.

Spike #1 measured one frame at 320x180 and produced a result that contradicts
the common claim that "delta-encoding SGR cuts half-block output by 30-50%".
It doesn't, for 24-bit output, because truecolour video has an effectively
unique (R,G,B) per cell. This spike normalises everything per *cell* and per
realistic terminal size, so the plan is built on bytes-per-cell, not vibes.

Key output: bytes per frame at 80x24, 160x50 and 320x90 terminals, and the
maximum sustainable frame rate at a 10 MB/s pty.

Run: python3 spikes/bench_cells.py
"""
import base64
import math
import zlib

from bench_backends import content, enc_blocks_naive, enc_blocks_delta, enc_sixel, \
    kitty, pack_halfblocks, rgb_plane, Frame

PTY_MBPS = 10.0  # a fast ssh pty / typical terminal read loop, bytes floor


def cells_for(cols, rows, halfblock=True):
    """pixel raster that fills the cell grid."""
    px_w = cols
    px_h = rows * (2 if halfblock else 1)
    return px_w, px_h


def measure(kind, cols, rows):
    w, h = cells_for(cols, rows)
    f = content(kind, w, h, 0)
    cells = pack_halfblocks(f, 2)
    n_cells = len(cells) * (len(cells[0]) if cells else 0)

    naive = len(enc_blocks_naive(cells))
    delta = len(enc_blocks_delta(cells, {"fg": -1, "bg": -1}))
    six = len(enc_sixel(f))
    kraw = len(kitty(rgb_plane(f), w, h))
    kz = len(kitty(rgb_plane(f), w, h, compress=True))
    kshm = len(kitty(rgb_plane(f), w, h, shm=True))
    return dict(cells=n_cells, naive=naive, delta=delta, sixel=six, kraw=kraw, kz=kz, kshm=kshm)


def fps(b, budget=PTY_MBPS * 1e6):
    return budget / b if b else 999


def main():
    sizes = [(80, 24), (160, 50), (320, 90)]
    kinds = ["gradient", "photo", "noisy"]

    print("=" * 96)
    print("BYTES PER FRAME (half-block raster filling the cell grid)")
    print("=" * 96)
    hdr = f"{'terminal':12} {'content':9} {'cells':>7} {'24bit':>10} {'B/cell':>8} {'delta%':>7} {'sixel':>10} {'kitty':>10} {'kitty+z':>10}"
    print(hdr)
    print("-" * 96)
    for (cols, rows) in sizes:
        for kind in kinds:
            m = measure(kind, cols, rows)
            print(f"{f'{cols}x{rows}':12} {kind:9} {m['cells']:7,} {m['naive']:10,} "
                  f"{m['naive'] / m['cells']:8.1f} "
                  f"{100 * (1 - m['delta'] / m['naive']):7.1f} "
                  f"{m['sixel']:10,} {m['kraw']:10,} {m['kz']:10,}")
        print()

    print("=" * 96)
    print("MAX SUSTAINABLE FPS at a 10 MB/s pty (the hard ceiling for any terminal player)")
    print("=" * 96)
    print(f"{'terminal':12} {'content':9} {'24bit':>8} {'sixel':>8} {'kitty':>8} {'kitty+z':>8} {'kitty-shm':>10}")
    print("-" * 96)
    for (cols, rows) in sizes:
        for kind in kinds:
            m = measure(kind, cols, rows)
            print(f"{f'{cols}x{rows}':12} {kind:9} {fps(m['naive']):8.1f} {fps(m['sixel']):8.1f} "
                  f"{fps(m['kraw']):8.1f} {fps(m['kz']):8.1f} {'unbounded':>10}")
        print()

    print("=" * 96)
    print("WHAT 24-bit HALF-BLOCK COSTS, BROKEN DOWN")
    print("=" * 96)
    m = measure("photo", 80, 24)
    per_cell = m["naive"] / m["cells"]
    print(f"  one cell = '\\x1b[38;2;198;119;96m' (19 B) + '\\x1b[48;2;...m' (19 B) + glyph (3 B)")
    print(f"  measured: {per_cell:.1f} bytes/cell including the per-row reset")
    print(f"  80x24 needs {m['cells']:,} cells -> {m['naive']:,} B/frame -> "
          f"{m['naive'] * 30 / 1e6:.1f} MB/s at 30 fps")
    print()
    print("  => 24-bit half-blocks at full grid is NOT viable above ~10-15 fps on ssh.")
    print("     Either go indexed 256-colour (halves bytes, costs fidelity),")
    print("     or cap the raster, or use a graphics protocol. This is the single")
    print("     most important constraint the design must respect.")

    print()
    print("=" * 96)
    print("INDEXED 256-COLOUR VARIANT (38;5;n instead of 38;2;r;g;b)")
    print("=" * 96)
    for (cols, rows) in sizes:
        w, h = cells_for(cols, rows)
        f = content("photo", w, h, 0)
        cells = pack_halfblocks(f, 2)
        n_cells = len(cells) * len(cells[0])
        # approximate: '#38;5;NNNm' = 8 bytes, x2 (fg+bg) + 3 glyph = 19 B/cell
        approx = n_cells * 19 + len(cells) * 4
        print(f"  {cols}x{rows}: {approx:>9,} B/frame  "
              f"({100 * approx / measure('photo', cols, rows)['naive']:.0f}% of 24-bit), "
              f"{fps(approx):5.1f} fps")
    print("  Note: 19 B/cell vs 38 B/cell is a 2x win for a real quality loss on")
    print("  gradients (banding). Worth offering as --color 256 for slow links.")

    print()
    print("=" * 96)
    print("SIXEL ENCODER COST (pure-Python reference; Rust/SIMD should be 20-50x faster)")
    print("=" * 96)
    import time
    for (cols, rows) in sizes:
        w, h = cells_for(cols, rows)
        for kind in kinds:
            f = content(kind, w, h, 0)
            t0 = time.perf_counter()
            n = len(enc_sixel(f))
            ms = (time.perf_counter() - t0) * 1000
            print(f"  {cols}x{rows} {kind:9} {n:>9,} B  {ms:8.1f} ms/encode "
                  f"({1000 / ms:5.1f} fps CPU ceiling, python)")
        print()
    print("  Fixed 6x7x6 cube is fast on smooth content, expensive on noise")
    print("  (every cell needs 3 quantise calls). Budget: the sixel path must be")
    print("  written in Rust with a band-parallel encoder, or cached.")


if __name__ == "__main__":
    main()
