# TermiPlayer — research and plan

*Written 2026-11-08. This is the document that answers: what is the market, what is
missing, what will we build, why is it better, and in what order.*

Companion documents:
- [`COMPETITORS.md`](COMPETITORS.md) — the landscape, with sources and numbers
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — modules, milestones, acceptance criteria, risks
- [`../spikes/RESULTS.md`](../spikes/RESULTS.md) — the measurements this plan rests on

---

## 1. Executive summary

The terminal media player market is **crowded on one axis and empty on another**.

Crowded: *rendering pixels into a terminal*. Six graphics protocols, four mature C
libraries, and a wave of 2026 Rust projects, all solving the same encoding problem with
different trade-offs. This is a solved, well-benchmarked space.

Empty: *being a player*. Not one terminal project combines video rendering, audio, a
persistent library, a queue and resume-where-you-left-off in a single binary. `ncplayer`
has no audio. `timg` has no audio. `rtv` has audio but no library. `cmus` and `termusic`
have beautiful libraries and no video at all.

TermiPlayer occupies the intersection: **one binary, offline, video + audio, with a
library that remembers.**

And it arrives with a technical thesis nobody else has articulated, because we measured
it rather than repeating folklore: **the pty's byte throughput, not the CPU, is the hard
constraint**, and a player must negotiate resolution against frame rate continuously.
Our spike shows a 320x90 truecolor raster caps out at ~9 fps over a 10 MB/s link. Every
competing project pretends otherwise, then silently drops frames.

---

## 2. The problem, precisely

A person who lives in a terminal wants to watch a downloaded lecture, or listen to an
album, without leaving it. Today they must choose:

| They do this | And it fails because |
|---|---|
| `mpv --vo=tct film.mkv` | Works, but there is no library, no resume, no subtitle comfort, and the output quality is whatever mpv's tct bolt-on gives |
| `ncplayer film.mkv` | Excellent picture, **no sound** |
| `timg film.mkv` | No sound, no seek as a player, designed as an image viewer |
| `cmus` / `termusic` | Perfect audio library; cannot show video |
| `tubetui` | Beautiful TUI + embedded video, but needs mpv + yt-dlp, and it is a YouTube client, not a local player |
| `rtv film.mkv` | Fast, correct, five backends — and it forgets the file the moment it exits |

The unmet need is not "better pixels". It is: **"treat my files as a collection, and
remember what I was doing."** That is what `cmus` gives for music and what nothing gives
for video in a terminal.

---

## 3. Market evidence

**Demand is real and current.** Searches conducted today surfaced, among many others:

| Project | ▲ | Signal |
|---|---|---|
| mpv | 37,993 | the substrate everyone builds on |
| spotify-player | 7,306 | proof people want rich media TUIs (but cloud-bound) |
| cmus | 6,286 | 20 years of terminal-library muscle memory |
| chafa | 5,390 | image→escape is mainstream |
| notcurses | 4,769 | best blitters, still no audio path |
| timg | 2,795 | widely packaged (Debian ITP), video is a side feature |
| termusic | 2,247 | audio library + podcasts + downloads, active (pushed 2026-11-05) |
| tubetui | 78 | a 2026 project proving TUI + embedded video is wanted |
| rtv | 7 | best terminal video *engine*, near-zero discoverability |
| framefeed, sugar-video, mpv-kitty | 0 | 2026 newcomers, all renderers or mpv wrappers |

Read the shape of that list: **every project above 2,000 stars is either an image/escape
tool, a substrate, or an audio-only library.** The video-in-terminal projects with real
engineering are all under a few hundred stars. The category has no flagship.

**Why the gap persists** (not an oversight, a structural cause):
- Renderers are C/C++ media projects; libraries are TUI/ergonomics projects. Different
  skills, different testing, different instincts.
- Renderers optimize single-frame latency; libraries optimize state and navigation.
  Neither team has time to learn the other's problem.
- Terminal graphics only became genuinely good (kitty protocol adoption in Ghostty,
  WezTerm, Konsole; Windows Terminal sixel in 1.22) recently. Before that there was no
  point building the library on top of mush.

**Why now is the right moment:** the rendering problem has become commodity. That means
effort spent on the library half is now the differentiator, not wasted on plumbing.

---

## 4. The thesis (three claims, each testable)

**Claim 1 — Scope: nobody ships both halves.**
Verified by the feature matrix in `COMPETITORS.md §2.3`. Testable by anyone in ten
minutes of README reading.

**Claim 2 — Throughput adaptivity is the missing innovation.**
Every competing player fixes a raster at startup and hopes. The spike proves that
assumption breaks: bytes-per-second to the terminal is a hard ceiling, and it varies by
an order of magnitude between a local kitty (64 B/frame via shm) and ssh blocks (1 MB/frame).
TermiPlayer measures it and renegotiates. Nobody else does this.

**Claim 3 — Zero-dependency audio is a real user win.**
`termusic` and `rtv` need mpv or GStreamer toolchains for some paths; `ncplayer` has no
audio. We decode in pure Rust (symphonia) and output through cpal, so an audio file plays
on a machine with no media stack installed. Testable on a fresh container.

**Corollary — we win on evidence, not adjectives.** This document quotes measured
numbers, including one that contradicts a competitor's headline claim. In a niche built
on folklore ("delta-encoding saves 30-50%"), the project that publishes real measurements
becomes the reference. That is the marketing strategy: be the one with the honest table.

---

## 5. Why the answer is not "just use mpv"

The strongest objection, taken seriously: mpv already plays video and audio in a
terminal via `--vo=tct` and `--vo=kitty`. Why build anything?

Because the *player* is the product, and mpv explicitly is not a library manager:

1. **mpv has no memory.** No watch state, no resume, no queue across invocations. This
   is not a missing flag; it is out of scope for them.
2. **Terminal output is mpv's least-loved path.** The request to replace the tct backend
   with notcurses blitters has been open since 2020. They will not prioritize it.
3. **Install friction.** mpv + yt-dlp + a config file is a toolkit, not a product.
4. **Configuration model mismatch.** Tuning terminal output means learning mpv's
   option surface; the right model is "it looks good, and `player doctor` tells me why".
5. **Distribution weight.** ~40 MB and 150-300 ms startup for a text raster.

What we *do* take from mpv is the correctness bar — and we take FFmpeg, which is the same
decode substrate mpv uses. We are not reimplementing decoders. We are building the layer
mpv refuses to build.

An honest counterpoint: if a user only ever wants to play one file, `mpv --vo=kitty` is
fine and we should say so in the README. Our value starts at "I have more than one file."

---

## 6. Target users and the jobs they hire us for

| Persona | Job to be done | What wins |
|---|---|---|
| **The ssh-native developer** | "watch the lecture I downloaded, in my terminal, without X" | it plays over ssh, adapts to the link, remembers my position |
| **The tiling-WM minimalist** | "my whole workflow is a terminal; media should not force a GUI" | keyboard-only, no window, no daemon |
| **The server / homelab admin** | "listen to my music library while I work in tmux" | audio path with no system deps, survives detach |
| **The low-power / old-laptop user** | "a video player that doesn't melt my machine" | 20 MB RAM, CPU decode only where needed, degrades instead of stuttering |
| **The accessibility / dumb-terminal user** | "no graphics support here, but I still need to follow along" | ASCII fallback that always works, subtitles in cell space |

Anti-persona, stated up front: people who want to watch 4K HDR on a big screen. We are
not competing with mpv's GUI output, and pretending otherwise would ruin the product.

---

## 7. Feature set for 1.0

**Must have (the 1.0 gate)**
- play local video (FFmpeg formats) and audio (pure Rust), correct A/V sync
- auto-selected best backend, with a working fallback chain down to ASCII
- adaptive resolution/frame-rate control on slow links
- library scan, resume, queue, playlists, search
- subtitles (embedded + external), chapters, HUD
- keyboard-first, remappable, with in-app help
- single binary, `cargo install`-able, no runtime external deps for audio

**Should have**
- thumbnails and waveforms in the library
- speed control with pitch preservation, gapless playback
- `--stats` / `player doctor` diagnostics
- MPRIS (optional feature) for media keys
- shell completions, man page

**Won't have (1.0)**
streaming services, DRM, GUI, editing, transcoding, plugins, sync across devices.

Full scope, milestones and acceptance criteria: `ARCHITECTURE.md §5`.

---

## 8. Competitive strategy

Positioning, in one line each:

- **against mpv:** "mpv is an engine. We are a player with a memory."
- **against ncplayer/timg/chafa:** "those are renderers. Try playing a video with sound."
- **against rtv:** "great engine, no library, and its headline optimization measurably
  doesn't work. Ours adapts to your actual bandwidth."
- **against cmus/termusic:** "the same library comfort, and it also shows video."
- **against tubetui/yute:** "no mpv, no yt-dlp, no account — your files, your machine."

Distribution plan (because in this niche, discoverability beats engineering):
1. Publish the **spike and its numbers first** — a "we measured the thing everyone
   assumes" post is the most shareable artifact this project can produce.
2. Demo GIFs per backend, recorded from a real terminal, in the README.
3. Package everywhere: AUR, Homebrew, `.deb`, winget, Termux notes.
4. Land in `awesome-tuis`, r/rust, r/commandline, HN "Show HN", Lobsters.
5. A `player doctor` that solves the top support question (tmux/terminal config) before
   it is asked.

---

## 9. Success metrics

| Horizon | Metric | Target |
|---|---|---|
| M1 | time to first frame, warm file | <120 ms |
| M1 | dropped frames, 720p on kitty | <1% |
| M2 | median A/V drift | ≤ ±10 ms |
| M3 | filter latency, 1,000 files | <50 ms |
| M5 | sustained fps after bandwidth throttled to 1/4 | ≥24 fps by resolution drop |
| M5 | sixel encode, 160x50 | <3 ms |
| M6 | unchanged rescan, 10,000 files | <2 s |
| Launch | stars in 30 days | 500 |
| Launch | "first playback" on 4 platforms | <3 min each |

Non-goals as metrics: resolution, because we optimize sustained smoothness — and
`--stats` should make that visible rather than aspirational.

---

## 10. Open questions to resolve in M0

These change the architecture, so they are answered with a real pty before M1:

1. **Real pty throughput.** The 10 MB/s assumption in the spike needs replacing with a
   measured curve per terminal (kitty, WezTerm, xterm, tmux, over ssh).
2. **Does blocks256 actually look acceptable to users?** The 2x byte win is real; the
   banding cost needs a human judgement call, so ship both and default per bandwidth.
3. **How reliable is `CSI 16t` cell-size probing across the top 10 terminals?** If it is
   flaky, the throughput controller must drive raster choice instead of a fixed guess.
4. **Sixel CPU in Rust.** If the 3 ms target misses, sixel becomes a
   "≤160x50 only" backend — decide before writing the encoder, not after.
5. **tmux passthrough defaults.** Does `allow-passthrough` work well enough on current
   releases to auto-enable, or must we always degrade to blocks inside tmux?
6. **Audio backend variance.** Does cpal+PipeWire need a different cushion than ALSA?
   Measure, don't guess.

---

## 11. Bottom line

There is a real, unoccupied position in this market, and it is not "faster encoder". It is
**"the terminal media player that is actually a player"** — with an evidence-backed
technical edge (throughput-adaptive rendering) that no competitor has, and a product
moat (library, resume, queue) that requires taste rather than cleverness to copy.

Twelve weeks to a 1.0 that is usable at every milestone. The build starts with M0: prove
the probe and the pty numbers, then make a video play.
