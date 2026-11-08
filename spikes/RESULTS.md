# Spike results

All numbers produced by the scripts in this directory, on 2026-11-08, on the sandbox
runner (Python 3.11, single core, no GPU, no audio device). They are **encoder-cost and
byte-count** measurements only. Nothing here is a claim about picture quality on your
terminal; that requires real footage and a real emulator, which is M1 work.

Reproduce:

```bash
cd spikes
python3 bench_backends.py     # per-frame bytes, one 320x180 frame, 3 content types
python3 bench_cells.py        # normalised per-cell, realistic terminal geometries
```

Why synthetic content: three controlled entropy levels let us say "this backend is cheap
on smooth content and expensive on noise" with a number attached, which real footage
cannot do reproducibly. `gradient` = smooth sky/anime, `photo` = pseudo-photographic with
a moving textured region, `noisy` = high-entropy (confetti/foliage/fire).

---

## 1. Bytes per frame, realistic terminal geometries

Raster fills the cell grid. Half-block = 1x2 pixels per cell.

| terminal | content | cells | 24-bit blocks | B/cell | delta saving | sixel | kitty raw | kitty zlib |
|---|---|---|---|---|---|---|---|---|
| 80x24 | gradient | 1,920 | 74,376 | 38.7 | 0.1% | 4,574 | 15,504 | 14,176 |
| 80x24 | photo | 1,920 | 71,301 | 37.1 | 0.1% | 4,228 | 15,504 | 7,852 |
| 80x24 | noisy | 1,920 | 73,849 | 38.5 | 0.1% | 13,275 | 15,504 | 15,536 |
| 160x50 | gradient | 8,000 | 309,870 | 38.7 | 0.0% | 7,004 | 64,608 | 60,274 |
| 160x50 | photo | 8,000 | 296,875 | 37.1 | 0.1% | 6,801 | 64,608 | 17,546 |
| 160x50 | noisy | 8,000 | 307,568 | 38.4 | 0.0% | 34,548 | 64,608 | 64,700 |
| 320x90 | gradient | 28,800 | 1,114,570 | 38.7 | 18.4% | 13,175 | 232,566 | 159,022 |
| 320x90 | photo | 28,800 | 1,067,412 | 37.1 | 0.0% | 15,145 | 232,566 | 52,114 |
| 320x90 | noisy | 28,800 | 1,106,897 | 38.4 | 0.0% | 88,940 | 232,566 | 232,878 |

### Finding 1 — delta-encoding SGR is a dead end for truecolor (the important one)

`rtv`'s README claims "delta-encoded (~30-50% fewer bytes)" for half-block cells. Our
measurement says **0.0-0.1% on photographic and noisy content**, and 18.4% only on a
synthetic smooth gradient.

The reason is arithmetic, not implementation: a truecolor cell costs ~37 bytes because it
emits `\x1b[38;2;R;G;Bm\x1b[48;2;R;G;Bm` + glyph, and in real footage both colours are
almost never equal to the previous cell's. There is nothing to elide.

**Consequence for the design:** we do not build a delta SGR encoder for truecolor.
Instead the blocks backend gets:
- an **indexed 256-colour mode**, measured at **51% of the 24-bit byte cost** (19 vs 37
  bytes/cell) — a real 2x win for slow links;
- **per-row colour reuse** only where a row genuinely repeats (borders, letterboxing,
  black bars) — cheap to implement, sometimes free;
- **raster negotiation** (Finding 2), which dominates both.

### Finding 2 — the binding constraint is the pty, not the CPU

Max sustainable fps at a 10 MB/s pty:

| terminal | 24-bit blocks | sixel | kitty raw | kitty zlib | kitty shm |
|---|---|---|---|---|---|
| 80x24 | 134 | 2,186 | 645 | 705 | ∞ (64 B/frame) |
| 160x50 | 32 | 1,427 | 155 | 570 | ∞ |
| 320x90 | **9** | 759 | 43 | 192 | ∞ |

**A 320x90 truecolor half-block raster cannot exceed ~9 fps over ssh.** This single number
justifies the adaptive throughput controller as a *core* feature rather than a nice-to-have.
sixel is 70x cheaper in bytes than 24-bit blocks at the same geometry.

### Finding 3 — kitty compression is content-dependent, so it must be adaptive

zlib vs raw base64 at 320x90: **4.46x** better on photo (52,114 vs 232,566), only
**1.46x** on gradient (159,022 vs 232,566), and **1.00x — slightly worse** on noise
(232,878 vs 232,566). At 160x50 the same spread is 3.68x / 1.07x / 0.99x, so the effect
scales with raster size as well as content. A player that always compresses wastes
CPU on busy scenes; one that never compresses wastes 4x the bandwidth. The controller
picks per frame from a cheap entropy estimate.

### Finding 4 — shared memory is the only free lunch

Local kitty at any resolution: a 64-byte escape naming a shm object. Use it whenever
`TIOCGWINSZ`/session detection says "same machine", and skip the whole bytes-per-frame
budget. This is the one path where resolution is genuinely unconstrained by the pty.

---

## 2. Encode cost (pure-Python reference implementation)

| terminal | content | sixel bytes | ms/encode | CPU ceiling (fps) |
|---|---|---|---|---|
| 80x24 | gradient | 4,574 | 7.2 | 138 |
| 80x24 | noisy | 13,275 | 15.5 | 64 |
| 160x50 | photo | 6,801 | 28.2 | 35 |
| 160x50 | noisy | 34,548 | 67.9 | 15 |
| 320x90 | photo | 15,145 | 102.9 | 10 |
| 320x90 | noisy | 88,940 | 264.3 | 3.8 |

Interpretation, being careful not to over-read a Python prototype:

- Sixel is **byte-cheap and CPU-expensive**. In Python it cannot hold 320x90 at 30 fps.
  M5's acceptance bar is a band-parallel Rust encoder with a SIMD quantiser under 3 ms at
  160x50; the 6.7x-35x headroom Rust should give us makes that plausible but **unproven
  until built**. Until it is proven, the plan's fallback is "sixel only up to 160x50; above
  that, use kitty or blocks".
- Noise content costs 3-6x more CPU than smooth content, so the encoder must budget by
  measured frame entropy, not by a fixed palette size.

---

## 3. What the spike did *not* establish

Being explicit, because these numbers will be quoted:

- **Not picture quality.** PSNR of a quantiser against its own input is meaningless; we
  skipped it rather than publish a fake number.
- **Not real throughput.** `write()` to a pty, terminal parser speed, and ssh RTT are not
  modelled. `10 MB/s` is a stated assumption for comparison, not a measurement of any
  real link. M0 must instrument a real pty.
- **Not decode cost.** FFmpeg decode, scaling and colour conversion dominate the budget;
  the spike starts after decode.
- **Not the GUI/terminal difference.** Cell pixel size, font aspect ratio and DPI change
  effective resolution in ways a byte count cannot capture.

Each of these is a milestone acceptance gate, listed in `docs/ARCHITECTURE.md §6`.

---

## Appendix: raw output

```
================================================================================================
BYTES PER FRAME (half-block raster filling the cell grid)
================================================================================================
terminal     content     cells      24bit   B/cell  delta%      sixel      kitty    kitty+z
------------------------------------------------------------------------------------------------
80x24        gradient    1,920     74,376     38.7     0.1      4,574     15,504     14,176
80x24        photo       1,920     71,301     37.1     0.1      4,228     15,504      7,852
80x24        noisy       1,920     73,849     38.5     0.1     13,275     15,504     15,536

160x50       gradient    8,000    309,870     38.7     0.0      7,004     64,608     60,274
160x50       photo       8,000    296,875     37.1     0.1      6,801     64,608     17,546
160x50       noisy       8,000    307,568     38.4     0.0     34,548     64,608     64,700

320x90       gradient   28,800  1,114,570     38.7    18.4     13,175    232,566    159,022
320x90       photo      28,800  1,067,412     37.1     0.0     15,145    232,566     52,114
320x90       noisy      28,800  1,106,897     38.4     0.0     88,940    232,566    232,878

================================================================================================
MAX SUSTAINABLE FPS at a 10 MB/s pty (the hard ceiling for any terminal player)
================================================================================================
terminal     content      24bit    sixel    kitty  kitty+z  kitty-shm
------------------------------------------------------------------------------------------------
80x24        gradient     134.5   2186.3    645.0    705.4  unbounded
80x24        photo        140.3   2365.2    645.0   1273.6  unbounded
80x24        noisy        135.4    753.3    645.0    643.7  unbounded

160x50       gradient      32.3   1427.8    154.8    165.9  unbounded
160x50       photo         33.7   1470.4    154.8    569.9  unbounded
160x50       noisy         32.5    289.5    154.8    154.6  unbounded

320x90       gradient       9.0    759.0     43.0     62.9  unbounded
320x90       photo          9.4    660.3     43.0    191.9  unbounded
320x90       noisy          9.0    112.4     43.0     42.9  unbounded

================================================================================================
WHAT 24-bit HALF-BLOCK COSTS, BROKEN DOWN
================================================================================================
  one cell = '\x1b[38;2;198;119;96m' (19 B) + '\x1b[48;2;...m' (19 B) + glyph (3 B)
  measured: 37.1 bytes/cell including the per-row reset
  80x24 needs 1,920 cells -> 71,301 B/frame -> 2.1 MB/s at 30 fps

  => 24-bit half-blocks at full grid is NOT viable above ~10-15 fps on ssh.
     Either go indexed 256-colour (halves bytes, costs fidelity),
     or cap the raster, or use a graphics protocol. This is the single
     most important constraint the design must respect.

================================================================================================
INDEXED 256-COLOUR VARIANT (38;5;n instead of 38;2;r;g;b)
================================================================================================
  80x24:    36,576 B/frame  (51% of 24-bit), 273.4 fps
  160x50:   152,200 B/frame  (51% of 24-bit),  65.7 fps
  320x90:   547,560 B/frame  (51% of 24-bit),  18.3 fps
  Note: 19 B/cell vs 38 B/cell is a 2x win for a real quality loss on
  gradients (banding). Worth offering as --color 256 for slow links.

================================================================================================
SIXEL ENCODER COST (pure-Python reference; Rust/SIMD should be 20-50x faster)
================================================================================================
  80x24 gradient      4,574 B       7.4 ms/encode (135.9 fps CPU ceiling, python)
  80x24 photo         4,228 B       6.7 ms/encode (148.9 fps CPU ceiling, python)
  80x24 noisy        13,275 B      15.0 ms/encode ( 66.6 fps CPU ceiling, python)

  160x50 gradient      7,004 B      28.6 ms/encode ( 34.9 fps CPU ceiling, python)
  160x50 photo         6,801 B      29.2 ms/encode ( 34.3 fps CPU ceiling, python)
  160x50 noisy        34,548 B      67.3 ms/encode ( 14.9 fps CPU ceiling, python)

  320x90 gradient     13,175 B     104.8 ms/encode (  9.5 fps CPU ceiling, python)
  320x90 photo        15,145 B     100.2 ms/encode ( 10.0 fps CPU ceiling, python)
  320x90 noisy        88,940 B     264.6 ms/encode (  3.8 fps CPU ceiling, python)

  Fixed 6x7x6 cube is fast on smooth content, expensive on noise
  (every cell needs 3 quantise calls). Budget: the sixel path must be
  written in Rust with a band-parallel encoder, or cached.
```

```
content    backend         bytes/frame  MB/s @30fps  ms/frame  fits ssh?
--------------------------------------------------------------------------
gradient   blocks-24bit      1,114,570        33.44     23.48         NO
gradient   blocks-delta        909,116        27.27     30.44         NO
gradient   sixel                13,175         0.40    102.75        yes
gradient   kitty-raw           232,566         6.98      0.38        yes
gradient   kitty-zlib          159,022         4.77      3.37        yes
gradient   kitty-shm                64         0.00      0.02        yes
           -> delta saves        18.4%

photo      blocks-24bit      1,067,412        32.02     22.62         NO
photo      blocks-delta      1,067,142        32.01     33.78         NO
photo      sixel                15,145         0.45     98.71        yes
photo      kitty-raw           232,566         6.98      0.37        yes
photo      kitty-zlib           52,114         1.56      1.15        yes
photo      kitty-shm                64         0.00      0.02        yes
           -> delta saves         0.0%

noisy      blocks-24bit      1,106,897        33.21     24.67         NO
noisy      blocks-delta      1,106,627        33.20     36.39         NO
noisy      sixel                88,940         2.67    243.32        yes
noisy      kitty-raw           232,566         6.98      0.40        yes
noisy      kitty-zlib          232,878         6.99      4.08        yes
noisy      kitty-shm                64         0.00      0.02        yes
           -> delta saves         0.0%

Sixel fidelity check (fixed 6x7x6 + Bayer vs truecolor source):
  source is truecolor; sixel path is a 252-colour ordered-dither
  approximation. PSNR of palette reconstruction: 99.0 dB (baseline, identical bytes)
```
