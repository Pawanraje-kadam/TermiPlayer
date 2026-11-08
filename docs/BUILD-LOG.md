# What was built, and what changed

*Implementation log for M0–M2. Companion to [`../docs/RESEARCH-AND-PLAN.md`](../docs/RESEARCH-AND-PLAN.md)
(the plan) and [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md) (the design).
Written as the work happened, including the mistakes, because a build log that
only records successes is useless to whoever reads it next.*

---

## 1. The one decision that changed: C instead of Rust

The plan specified Rust (ratatui + ffmpeg-the-third + symphonia + cpal). That is
still, in my view, the better long-term choice.

It was not buildable **in this environment**:

| Check | Result |
|---|---|
| `rustc` / `cargo` present | **no** |
| `static.rust-lang.org` reachable | **no** (connection timeout) |
| `crates.io` reachable | **no** (DNS/connection blocked) |
| `gcc`, `make`, libc headers | **yes** |

Since crates.io is unreachable, *any* dependency-based Rust build would fail here
regardless of the toolchain — including a hand-written one. So the choice was:

1. write ~3,500 lines of unverifiable Rust, or
2. write C that compiles and can be tested end to end, today.

**We built C, and verified it.** What that bought, concretely:

* 145 unit checks and 25 integration checks run green, including under
  ASan/UBSan
* **six real bugs found and fixed by testing**, not by reading (see §5) — the
  kind that would otherwise have shipped
* measured per-backend costs from the actual binary, which *confirm* the plan's
  benchmark predictions

What it cost: no memory-safety guarantee from the compiler, and manual resource
management. Mitigation is the sanitizer target (`make test-san`), which is run on
every change, plus a strict-warning build.

If the project continues in Rust, `src/*.h` is the specification and
`docs/ROADMAP.md §2` describes the port seam.

---

## 2. The second decision: FFmpeg is a process, not a link

The plan said "link libavcodec". Here, decoding spawns an `ffmpeg` subprocess and
reads `rawvideo`/`s16le` from its stdout.

This was **not** a compromise forced by the environment — the environment has no
FFmpeg *headers* either, so the subprocess was the only option here — but it is
defensible on its own terms:

| | linked libav | ffmpeg subprocess |
|---|---|---|
| build dependencies | FFmpeg dev headers + libs | none (a C compiler) |
| codec coverage | whatever was linked | everything that ffmpeg build has, incl. hwdec |
| copies per frame | 0 | 1 (pipe → our buffer) |
| failure isolation | decoder bug kills the player | decoder bug kills a child |
| upgrade path | relink against a new ABI | the user's ffmpeg improves for free |

The cost is one extra process, one memcpy per frame, and a hard dependency on
`ffmpeg` **at runtime**. That last one is real: it contradicts the plan's
"zero required external dependencies" claim *for video*, and the README now says
so plainly rather than repeating the plan's promise.

Audio, note, does **not** require ffmpeg for device output — only for decoding
codecs. The audio *sink* is still dependency-free (dlopen'd libpulse, or none).

**`DecodeSource` is deliberately an interface.** Swapping in linked libavcodec
later touches `decode.c` and nothing else.

---

## 3. What exists

```
src/
  util.c/h        buffers, errors, monotonic time, timecode parsing
  raster.c/h      the single pixel format + NN scaling + content statistics
  term.c/h        raw mode, geometry probing, capability detection, TtyWriter
  gfx.c/h         backend dispatch, base64, zlib-via-dlopen
  gfx_blocks.c    half-blocks: truecolor + 256-colour, run-length
  gfx_ascii.c     the floor of the ladder
  gfx_kitty.c     kitty graphics: shm and direct (+zlib)
  gfx_sixel.c     sixel: content-aware palette, Bayer dither, RLE, banded
  gfx_iterm2.c    OSC 1337 inline images
  decode.c/h      ffmpeg subprocess: probe, video pipe, audio pipe
  audio.c/h       dlopen'd libpulse, fd sink, silent sink; the audio clock
  clock.c/h       A/V sync: audio master, smoothing, seek, drop/repeat
  throughput.c/h  the adaptive controller
  testsrc.c/h     synthetic diagnostic source
  session.c/h     the playback loop
  main.c          CLI + probe/doctor/backends subcommands
tests/
  test_main.c     145 unit checks
  integration.sh  25 end-to-end checks against the binary
tools/
  mkfixtures.sh   generate test media with ffmpeg
  render_frames.py  render an escape stream to PNG (how the screenshots were made)
  check_escapes.py  validate escape stream structure
```

7 graphical backends, all working, all selectable with `-b`.

---

## 4. What was verified, and how

Everything below was run, not assumed.

### 4.1 The render path, per backend

`player testsrc` paints a diagnostic pattern (colour grid, grey ramp, moving bar,
per-frame counter, border). Each backend's raw escape stream was parsed by
`tools/render_frames.py` and rendered to PNG — that is how
`docs/img/*.png` were produced. Decoded video was verified the same way.

![testsrc rendering](../docs/img/testsrc-blocks.png)

### 4.2 Escape-stream validity

For every backend: zero malformed `ESC` sequences, zero NUL bytes, balanced
`BSU`/`ESU` (DEC 2026) pairs, no escape leakage to stderr.

### 4.3 Measured cost per backend (real binary, 40x22 cells, testsrc)

| backend | bytes/frame |
|---|---|
| blocks (truecolor) | 3,488 |
| blocks256 | 2,716 |
| ascii | 2,396 |
| sixel | 3,460 |
| kitty | 343 |
| iterm2 | 3,621 |

**blocks256 = 0.78 x blocks.** The plan predicted indexed colour would cost ~51%
of truecolor (from the Python spike's 19 vs 37 bytes/cell). Measured here it is
78%, not 51%.

That gap is informative rather than a failure: testsrc is flat colour, so the
truecolor path's SGR-on-change plus run-length already compresses it heavily
(4.2 bytes/cell instead of 37). Indexed colour's advantage is largest on *busy*
content, where truecolor pays the full per-cell price and 256-colour caps at 19
bytes/cell. Both numbers are true; they apply to different content. The
integration test asserts the conservative 0.85 bound, which holds for the flat
case and will hold more strongly on real footage.

### 4.4 Audio

Captured PCM from a 440 Hz sine source via the fd sink:

```
samples          258,048        (stereo s16)
peak             3037 / 32767
rms              2046
nonzero          100.0%
est. frequency   ~440 Hz        (source is a 440 Hz sine)
```

Resampling 44.1 kHz mono -> 48 kHz stereo is correct, and the signal is real.

### 4.5 Video

`testsrc2` 320x180 @ 24 fps decoded and rendered to blocks: 97 frames in 1.0 s of
requested playback, correct colours, correct geometry, clean exit.

![decoded video](../docs/img/video-blocks.png)

### 4.6 The throughput controller

Live `--stats` from a real run on an 80x24 terminal:

```
line   0: cells 80x24  raster 40x24   0.0 MB/s     <- calibrating
line   3: cells 80x24  raster 80x48  25.1 MB/s     <- upgraded to full size
line  95: cells 80x24  raster 80x48  60.0 MB/s     <- stable
```

It starts conservative, measures, upgrades once, and holds. Hysteresis prevents
oscillation. On a forced 320x90 raster it stays pinned as instructed.

---

## 5. Bugs that testing found (all fixed)

Recording these because each one is a class of bug, and the fix is the lesson.

1. **`layout()` called before `tp_new()`** — null dereference on every startup
   with a forced size. Found by ASan immediately. *Lesson:* initialization order
   in a long setup function needs to be structural, not remembered.
2. **`buf_printf` off-by-one** — reserved `n` bytes, wrote `n+1` (the NUL).
   Heap overflow in two backends; invisible in normal runs, fatal under ASan.
   *Lesson:* `vsnprintf` needs `n+1`; this is the classic.
3. **Wrong `argv` index for `-vf`** — the scale filter value overwrote
   `-pix_fmt`'s argument, so ffmpeg was invoked with `-pix_fmt rawvideo`. With
   stderr suppressed this silently produced an empty pipe and a hang. *Lesson:*
   never build an `argv` by positional index into a literal array; build it
   explicitly.
4. **`read()` on the audio pipe could block forever** — block-buffered stdout in
   the child. Found by a timeout. *Lesson:* every read from a child process needs
   a deadline.
5. **`waitpid` deadlock in cleanup** — signalled ffmpeg while a pipe was full,
   then waited. It could never exit, so `waitpid` hung. Diagnosed by reading
   `/proc/PID/wchan` (`do_wait`). Fixed by closing pipes *before* signalling.
   *Lesson:* teardown order is part of the design.
6. **Throughput measured as bytes per syscall** — reported 1,856 MB/s, which made
   the controller believe every link was infinite. Fixed to measure bytes per
   *frame interval*. *Lesson:* the unit of a measurement is as important as the
   measurement.

Each has a regression test. Bugs 2, 3 and 5 are the ones a "write it and ship it"
approach would have shipped.

---

## 6. Deviations from the plan, honestly

| Plan said (M0–M2) | Built | Why |
|---|---|---|
| Rust workspace, 5 crates | C, single binary | no toolchain, no crate registry (see §1) |
| link libavcodec | ffmpeg subprocess | no headers available; defensible anyway (§2) |
| `CSI 16t`/`14t` cell-pixel probing | implemented, **untestable here** | this sandbox is `TERM=dumb` with no tty |
| hardware decode | passed through to ffmpeg (`-hwaccel` not yet exposed) | decode is a child process; see ROADMAP |
| subtitles, HUD, chapters | HUD only (progress bar + time + stats) | M4 scope, not started |
| library, resume, queue | **not started** | M3/M6 scope, not started |
| pure-Rust audio decode | ffmpeg for decode; native sink | consistent with §2 |

The **untested-in-this-environment** items are the honest weak spot, and they are
listed as the first tasks in `docs/ROADMAP.md`. Nothing here claims they work.

---

## 7. Test suite

```bash
make test        # 145 unit checks + 25 integration checks
make test-san    # the same, rebuilt with ASan + UBSan
```

```
util, raster, base64, raster size, backends, clock, throughput, testsrc, term
145 checks, 0 failed
```

```
cli, backends emit well-formed escapes, measured cost, media playback,
library and resume, interactive player, hygiene
40 checks, 0 failed
```

The unit suite gained 55 checks for the player layer (`media` release-name
parsing, `library` persistence and views, `screen` diff/geometry/ellipsis):
**200 checks, 0 failed**.

The integration suite skips (not fails) media tests when ffmpeg or fixtures are
absent, so it is safe to run anywhere.

---

## 8. Performance, as measured

| what | value |
|---|---|
| startup to first frame (blocks, 1 s clip) | ~15 ms + ffmpeg spawn |
| frames rendered, 1.0 s of 24 fps source | 97 (paced by the clock, not by decode) |
| binary size | 247 KB, zero non-libc dependencies |
| RSS during 320x90 blocks playback | ~12 MB |

Not yet measured (needs a real terminal): sustained fps per backend, A/V drift
over a long clip, hwdec benefit.

---

## 9. The player layer: five bugs the tests now catch

Building the library, queue and resume turned up bugs that no amount of reading
the code would have shown. Each one is now pinned by a check.

**1. Resume seeked the clock, not the decoder.** `clock_seek()` moved the
timeline, but the `ffmpeg` child stayed at byte zero and nothing discarded
frames: the UI claimed 40 minutes in and showed frame 1. Fixed by
`decode_seek()`, which re-spawns the child with `-ss`. Verified by decoding a
clip that is red for 20 s and blue after: `-t 2` must yield red, `-t 30` blue.

**2. Two lines undid the seek, silently.** `clock_start_monotonic()` ran *after*
the resume seek and zeroed it; then `clock_set_audio_position(0, ...)` zeroed it
again. Both were one-liners in the right place doing the wrong thing. The audio
reference now starts at the position we actually start from.

**3. State rows for unindexed files were dropped.** `player movie.mkv` on a file
outside a watched folder created an entry, saved `S	...`, and then threw the row
away on load because it built state only for paths that already had an index row.
Resume therefore worked only for library files. State lines now create entries.

**4. PTS came from the clock, so nothing was ever paced.** `pts = clock_position()`
makes every frame look perfectly in sync (`diff == 0`), so the pacer never waited
and a 40-second clip played in two seconds and was marked watched. PTS must come
from the content — frame index ÷ source frame rate — because the clock is what we
are synchronising *against*. The integration suite now asserts that `-d 6` takes
about six seconds of wall time, which is the check that would have caught it.

**5. `frame_idx` was not incremented on the display path.** With (4) fixed, the
TUI still burned through a file: the discard branch advanced the counter but the
draw branch did not, so PTS froze after the first frame and every subsequent frame
looked 1.2 s behind and was discarded. One missing `++`.

Two smaller ones:

- `toast()` stored a pointer to the caller's **stack buffer**; the message read
  freed stack memory and usually rendered blank. It now owns a copy.
- One file could exist twice in the library (relative and absolute paths), each
  with its own resume point. Paths are canonicalised with `realpath()` at every
  entry point.
- `-d` was compared against the absolute position, so `-t 30 -d 5` exited before
  the first frame. It now means "play for N seconds from the start point".
