# Competitor landscape

*Researched 2026-11-08. Star counts are GitHub API values on that date; claims about
other projects' internals are taken from their own READMEs/source and marked as such.
Where something is unverified, it says so.*

---

## 1. What "terminal media player" actually means

Two completely different problems get named the same thing, and conflating them is why
this market feels crowded but unsatisfying:

**(A) Renderers.** "Given a frame, how do I put pixels in a terminal?"
This is a solved-ish, well-documented, heavily optimized problem. Six graphics protocols,
a dozen encoders, three mature C libraries.

**(B) Players.** "Given a media library and a human, how do I do the right thing without
making them think?"
Playlists, resume, queue order, subtitles, search, remembering. This is not solved at
all in the terminal, because everyone solving (A) ran out of energy before (B).

Every competing project listed below is a strong answer to (A) and a partial or absent
answer to (B). That is the whole opportunity.

---

## 2. The field

### 2.1 Terminal video renderers (the (A) specialists)

| Project | ▲ | Lang | Video | Audio | Library | Notes |
|---|---|---|---|---|---|---|
| [mpv](https://github.com/mpv-player/mpv) `--vo=tct/-kitty/-sixel` | 37,993 | C | ✅ | ✅ | ❌ | The reference. Uses libmpv, so it *is* a full player — but the terminal output path is a bolt-on. |
| [notcurses](https://github.com/dankamongmen/notcurses) / `ncplayer` | 4,769 | C | ✅ | ❌ | ❌ | Best-in-class blitters (octants, quadrants, sixel, kitty). **`ncplayer` is video-only** — its demos use only swscale/avformat, no audio path. |
| [chafa](https://github.com/hpjansson/chafa) | 5,390 | C | images/GIF | ❌ | ❌ | The gold standard for *image*→escape conversion. Not a video player. |
| [timg](https://github.com/hzeller/timg) | 2,795 | C++ | ✅ short | ❌ | ❌ | Great image viewer, plays video as a frame sequence. No audio, no seeking as a player. |
| [rtv](https://github.com/Santitub/rtv) | 7 | Rust | ✅ | ✅ (cpal) | ❌ | The closest technical competitor. 5 backends, ffplay-style audio clock, hwdec, HUD. |
| [framefeed](https://github.com/furymob-git/framefeed) | 0 | Rust | ✅ | ⚠️ "audio still missing" | ❌ | Alpha. Good architecture doc for the kitty/sixel encoding path. |
| [mpv-kitty](https://github.com/safzanpirani/mpv-kitty) | 0 | Rust | ✅ | ✅ (via mpv IPC) | ❌ | KitKat-style embedding library; wraps mpv. Genuinely good idea, tiny reach. |
| [tubetui](https://github.com/polblancoo/tubetui) | 78 | Rust | ✅ | ✅ (via mpv) | ✅ (YouTube) | Shows TUI + embedded kitty video is what people want. Requires mpv + yt-dlp. |
| [sugar-video](https://github.com/sugarcraft/sugar-video) | 0 | PHP | ✅ | ❌ | ❌ | Interesting only as evidence of demand breadth. |

### 2.2 Terminal audio players (the (B) specialists)

| Project | ▲ | Lang | Notes |
|---|---|---|---|
| [cmus](https://github.com/cmus/cmus) | 6,286 | C | 20 years old, still the benchmark for a terminal *music library*. Zero video. |
| [termusic](https://github.com/tramhao/termusic) | 2,247 | Rust | Triple audio backend (Symphonia / mpv / GStreamer), podcasts, yt-dlp downloads, tag editor. **The best terminal audio player; no video at all.** |
| [spotify-player](https://github.com/aome510/spotify-player) | 7,306 | Rust | Excellent TUI ergonomics. Requires a Spotify account — the opposite of offline. |

### 2.3 The gap, drawn

```
                    video  audio  library
mpv --vo=tct          ✅     ✅      ❌
ncplayer / timg       ✅     ❌      ❌
chafa                 ✅     ❌      ❌
rtv                   ✅     ✅      ❌
cmus / termusic       ❌     ✅      ✅
tubetui               ✅     ✅      ~ (YouTube only, needs mpv+yt-dlp)
─────────────────────────────────────────
TermiPlayer           ✅     ✅      ✅      ← nobody is here
```

The empty row is not empty because it's hard to think of. It's empty because it needs
**both** halves done well, and the two halves live in different codebases with different
cultures (media engineering vs. TUI engineering).

---

## 3. Where each competitor is actually weak

### 3.1 mpv `--vo=tct` — the thing everyone compares against

**Strength:** you get a real player for free; 37k stars of format support.

**Weaknesses that are *structural*, not fixable by mpv:**
- `--vo=tct` is a text output driver that emits per-cell SGR; by definition it cannot
  do the things `--vo=kitty` can.
- The terminal VOs are maintained as an afterthought. The `vo_tct` path has a known
  long-standing request to be replaced by notcurses blitters ([mpv#8344](https://github.com/mpv-player/mpv/issues/8344), open since 2020).
- mpv has no library management whatsoever. You pass it a file; it plays it; it forgets.
- Startup is heavy (~150-300 ms per `rtv`'s measurement) and the binary is large
  (~40 MB stripped per the same source).
- Users must *learn mpv* to configure the terminal output, which is a different mental
  model from "make it look good in my terminal".

**What we take from it:** the correctness bar (A/V sync, seek semantics) and the
recognition that FFmpeg is the only realistic decode substrate.

### 3.2 notcurses / `ncplayer`

**Strength:** the best terminal blitters that exist. Octant (4x2 px/cell), sextant,
quadrant, full sixel palette adaptation, kitty sprites with z-order.

**Weakness:** it is a C library for building TUIs that happens to have a demo video
player. `ncplayer` has no audio (it links swscale/avformat, not an output device), no
queue, no library, no resume, no subtitles worth mentioning, and no packaging story
beyond "install the dev library". Its own docs describe terminals, not playback.

**What we take from it:** the blitter quality target. If our blocks output looks worse
than `ncplayer`'s on the same terminal, we have failed.

### 3.3 rtv (the closest technical competitor)

This is the one to study, because it is the one doing the engineering right.

**What it claims** (from its README, source-verified by its author against mpv's source):
- 20-40 ms startup vs mpv's 150-300 ms
- audio master clock with ffplay's `compute_target_delay` thresholds
- shm for local kitty, zlib for remote, iTerm2 OSC 1337, sixel, blocks, ASCII
- **"delta-encoded (~30-50% fewer bytes)"** for half-blocks
- adaptive raster from real cell pixel size via `CSI 16t`/`14t`

**Where it is weak:**
- **No library, no queue, no resume.** Its HUD is a progress bar. It is a renderer with
  audio, exactly as its own tagline says.
- **The delta-encoding claim is measurably false for real video.** Our spike
  (§4.1) finds 0.0-0.1% savings, because truecolor footage has a unique colour per cell
  so there is nothing to reuse. It shows up as 18% on a synthetic pure gradient — a
  case that does not occur in footage. Building on this assumption means optimizing the
  wrong thing.
- Single-file invocation model. No concept of "my stuff".
- 7 stars: the engineering is admired by almost nobody because nobody can find it.

**Verdict:** the strongest *engine*, with the weakest *product*. Beating it is not about
being faster at encoding; it is about being the thing you leave open all day.

### 3.4 termusic / cmus (the library leaders)

**Strength:** the library model. termusic has podcasts, tag editing, downloads, three
audio backends. cmus has 20 years of muscle memory.

**Weakness:** *no video, at all.* Not a rendering limitation — an architectural one.
Both were designed around an audio sink and a track queue; there is no frame pipeline,
no graphics backend, no raster sizing, no A/V clock. Adding video to either is a rewrite.

**What we take from them:** the TUI information architecture (library tree / queue /
now-playing is the correct three-pane model) and the keybinding discipline.

### 3.5 The Rust newcomers (framefeed, tubetui, mpv-kitty, yute, rs-pug)

The 2026 wave proves demand: several independent people built terminal video this year.
All of them are either (a) alpha-stage encoders, or (b) thin TUIs over an external mpv
process. None ships a single self-contained binary that manages a local library.

**Lesson:** the market is not asking for another encoder. It is asking for the thing to
be *finished*.

---

## 4. Measured evidence

Method: [`spikes/bench_backends.py`](../spikes/bench_backends.py) +
[`spikes/bench_cells.py`](../spikes/bench_cells.py). Pure Python reference encoders,
synthetic content at three entropy levels (smooth gradient / pseudo-photographic /
high-entropy noise), three terminal geometries. Full tables in
[`spikes/RESULTS.md`](../spikes/RESULTS.md).

### 4.1 Delta-encoding SGR does not work on real video

| terminal | content | 24-bit bytes/frame | B/cell | delta saving |
|---|---|---|---|---|
| 80x24 | gradient | 74,376 | 38.7 | **0.1%** |
| 80x24 | photo | 71,301 | 37.1 | **0.1%** |
| 160x50 | photo | 296,875 | 37.1 | **0.1%** |
| 320x90 | photo | 1,067,412 | 37.1 | **0.0%** |
| 320x90 | gradient | 1,114,570 | 38.7 | 18.4% |

The one case where delta pays off (a synthetic smooth gradient) does not occur in real
footage. **Conclusion: don't build a delta SGR encoder for truecolor; it is pure
complexity for no gain.** Instead:
- emit indexed (`38;5;n`) colour on slow links — **measured 51% of the 24-bit size**,
  a real 2x win;
- cap the raster and negotiate it against frame rate (§4.2);
- use a graphics protocol wherever possible.

This is the kind of thing that only shows up when you measure, and it changes the
architecture of the blocks backend.

### 4.2 The real constraint: bytes per frame, not CPU

At a 10 MB/s pty ceiling:

| terminal | 24-bit max fps | sixel max fps | kitty max fps |
|---|---|---|---|
| 80x24 | 134 | 2,186 | 645 |
| 160x50 | 32 | 1,427 | 155 |
| 320x90 | **9** | 759 | 43 |

Nobody in this space states this plainly: **a 320x90 truecolor half-block raster cannot
exceed ~9 fps over ssh.** Any player that promises "full resolution, smooth, over ssh"
is lying or dropping frames silently.

Our answer — the adaptive throughput controller — is the direct consequence: the player
must treat *bytes per second* as a first-class resource alongside CPU.

### 4.3 Sixel is cheap in bytes and expensive in CPU

Sixel at 320x90 "photo" content: 15,145 B/frame (vs 1.07 MB for 24-bit blocks — a 70x
reduction) but 103 ms to encode in pure Python. Band-parallel Rust with a SIMD quantiser
should bring that under 3 ms, which is the M5 acceptance bar. Noise content is 264 ms and
89 KB even in Python: sixel must be *content-aware*, falling back to fewer palette entries
when the frame is busy.

### 4.4 Kitty shared memory is effectively free

Local kitty: a 64-byte escape per frame regardless of resolution. Remote: zlib beats raw
base64 by up to 4.4x on photographic content (52,114 vs 232,566 B) and **not at all** on
noise. So the remote path needs the same content-aware decision as sixel.

---

## 5. Positioning statement

> TermiPlayer is the first terminal media player that is a **player**, not a renderer:
> one binary, video and audio, offline, with a library that remembers where you stopped,
> and a graphics path that measures your actual connection and picks a resolution you can
> actually sustain.

Three claims, each testable:
1. **Only player with both halves** — video rendering *and* library/queue/resume.
2. **Only player that adapts to throughput** — measured pty bandwidth drives resolution.
3. **Only player with zero required external dependencies** for audio — pure-Rust
   decode + cpal, no mpv to install.

---

## 6. What could still beat us

Honest risks, expanded in `ARCHITECTURE.md §7`:

- **mpv adds a real TUI.** Unlikely (out of scope for them) but would erase our
  convenience advantage; we'd still win on the library.
- **Terminals standardize on kitty graphics everywhere** (including tmux/zellij). Then
  the graphics-protocol engineering becomes commodity — which strengthens our bet on the
  library half being the moat.
- **A funded team ships a polished competitor.** The 2026 wave shows the interest is real.
  Speed to a *usable* 1.0 matters more than perfection.
- **FFmpeg-in-Rust stalls.** Mitigated: audio path is already pure Rust, so the fallback
  is "audio always works, video needs FFmpeg libs".
