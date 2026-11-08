# TermiPlayer — architecture & build plan

Companion to [`RESEARCH-AND-PLAN.md`](RESEARCH-AND-PLAN.md) (the *why* and *for whom*)
and [`COMPETITORS.md`](COMPETITORS.md) (the *against whom*). This document is the *how*:
modules, data flow, milestone scope with acceptance criteria, and the risk register.

---

## 1. Design principles

1. **Two clocks, one truth.** Audio is the master clock (as in ffplay and rtv). Video
   frames are dropped or repeated to meet it, never the reverse. If an underpowered
   terminal cannot keep up, the picture degrades visibly and audio never stutters.
2. **The pty is a network.** Treat bytes-to-terminal as the scarce resource that it is.
   Measure it, budget against it, and negotiate quality continuously. (Our spike shows
   a 320x90 truecolor raster caps out at ~9 fps at 10 MB/s.)
3. **Three layers, cleanly separated.** Decode knows nothing about terminals. Graphics
   backends know nothing about codecs. The TUI knows nothing about escape sequences.
   This is what lets the same core serve a one-shot `player file.mkv` and the full TUI.
4. **Degrade, never fail.** Every backend has a fallback chain that ends in ASCII on a
   `TERM=dumb` terminal. No configuration should be able to produce a blank screen.
5. **Ship usable at every milestone.** After M1 you can watch a video. After M3 you can
   browse. There is never a "come back in six weeks" state.

---

## 2. Stack decisions

| Need | Choice | Why | Rejected |
|---|---|---|---|
| TUI | `ratatui` + `crossterm` | De-facto standard, immediate-mode, easy to unit-test by rendering to a `TestBackend` | notcurses (C FFI, opinionated cell model), cursive (retained, slower) |
| Video decode | `ffmpeg-the-third` (FFmpeg 7.x bindings) | Only realistic path to AV1/HEVC/VP9/hwdec; same substrate rtv uses | pure-Rust video (maturing but no hwdec, patchy AV1), GStreamer (heavy, plugin hell) |
| Audio decode | `symphonia` | Pure Rust, no system deps, gapless-aware, ~±15% of FFmpeg speed | FFmpeg audio (adds a hard libav dependency to the audio-only path) |
| Audio output | `cpal` | WASAPI/CoreAudio/ALSA/PulseAudio/PipeWire via one API, gives us the playback clock | portable-atomic clocks, `rodio` (adds a sink abstraction we don't want under a custom clock) |
| Resample | `libswresample` (via ffmpeg-the-third) or `rubato` | Must convert any layout/rate to device native | — |
| Library DB | `rusqlite` (bundled SQLite) | Single file, no server, FTS5 for search, portable | `sled` (no SQL/FTS), JSON files (no atomicity) |
| Config | `serde` + `toml` | Obvious | — |
| CLI | `clap` (derive) | Completions and man page for free | — |
| Logging | `tracing` to a file only | Never write to the tty; it corrupts the frame | — |
| MPRIS | `mpris-server` (optional feature) | Media keys / desktop integration for audio | — |

Deliberately **not** dependencies: no `libsixel` (we write the encoder, ~400 lines, and
avoid a C dep + its per-frame histogram cost), no `mpv` process, no `chafa`, no
`yt-dlp` in 1.0.

---

## 3. Architecture

```
                       ┌──────────────────────────────────────────┐
   path / URL ────────►│  demux  (ffmpeg-the-third)               │
                       │  streams · packets · pts · hwdec probe   │
                       └───────────┬───────────────┬──────────────┘
                                   │ video         │ audio
                                   ▼               ▼
                     ┌──────────────────┐  ┌───────────────────┐
                     │ video decode     │  │ audio decode      │
                     │ (ffmpeg, thread  │  │ (symphonia, or    │
                     │  pool, bounded   │  │  ffmpeg fallback) │
                     │  frame channel)  │  │ → resample → f32  │
                     └────────┬─────────┘  └─────────┬─────────┘
                              │ Frame{yuv/rgb,pts}    │ samples
                              ▼                       ▼
                     ┌──────────────────┐  ┌───────────────────┐
                     │ scale + convert  │  │ cpal output sink  │◄── volume, EQ,
                     │ (swscale, SIMD)  │  │ ring buffer       │    speed
                     └────────┬─────────┘  └─────────┬─────────┘
                              │ RGB raster            │ playback position
                              ▼                       ▼
        ┌──────────────────────────────────────────────────────────────┐
        │                    ClockController                           │
        │   master = audio position (or monotonic if no audio)         │
        │   target_delay = pts - clock  → drop / repeat / hold         │
        └───────────┬──────────────────────────────────┬───────────────┘
                    │ frame due                        │ budget feedback
                    ▼                                  ▼
        ┌───────────────────────────┐      ┌───────────────────────────┐
        │  Renderer (backend)       │      │ ThroughputController      │
        │  kitty · iterm2 · sixel   │◄─────│ measures B/s, decode ms,  │
        │  blocks24 · blocks256     │      │ frame entropy → picks     │
        │  ascii                    │      │ (backend, raster, fps)    │
        └───────────┬───────────────┘      └───────────────────────────┘
                    │ escape bytes
                    ▼
        ┌───────────────────────────┐      ┌───────────────────────────┐
        │  TtyWriter (single thread,│      │  Overlay composer         │
        │  buffered, BSU/ESU wrap,  │◄─────│  subtitles · HUD · toast  │
        │  resize BSU-safe)         │      │  (cell grid, on top)      │
        └───────────────────────────┘      └───────────────────────────┘
                    │
                    ▼
        ┌───────────────────────────┐      ┌───────────────────────────┐
        │  ratatui TUI (views)      │◄────►│  Library (SQLite)         │
        │  library · queue · now-   │      │  files, watch state,      │
        │  playing · settings       │      │  playlists, FTS5          │
        └───────────────────────────┘      └───────────────────────────┘
```

### Crate layout (workspace)

```
termiplayer/
├── crates/
│   ├── tp-core/      decode, decode threads, clock, throughput controller
│   ├── tp-render/    backends + encoders + terminal probe (no TUI, no decode)
│   ├── tp-library/   SQLite index, scan, playlists, watch state, FTS
│   ├── tp-tui/       ratatui views, keymap, config; depends on the three above
│   └── player/       the binary: CLI parsing, wiring, `player file.mkv` fast path
├── spikes/           measured encoders (this repo, now)
├── docs/
└── tests/fixtures/   3 small CC0 clips: smooth / busy / dark-with-subtitles
```

`tp-core` and `tp-render` must not depend on `tp-tui`; that keeps a `--no-tui`
one-shot mode and a future `--gui` possible without refactoring (this is exactly
rtv's mistake/feature, and it's cheap to get right at the start).

### Key interfaces (sketch, not final API)

```rust
// tp-render
pub trait Backend {
    fn id(&self) -> BackendId;
    fn probe(&self) -> bool;                       // env + tty query, cached
    fn max_raster(&self, cells: CellGrid) -> (u32, u32);
    fn draw(&mut self, frame: &Raster, area: Rect, out: &mut Vec<u8>);
    fn draw_rgb_rect(&mut self, r: &Rgba, area: Rect, out: &mut Vec<u8>);
    fn clear(&mut self, area: Rect, out: &mut Vec<u8>);
    fn present_mode(&self) -> PresentMode;         // Retransmit | Place | Cached
}

// tp-core
pub struct ClockController { /* audio pos, smoothing, target_delay */ }
pub enum FrameDecision { Show(Duration), Drop, Hold(Duration) }

pub struct ThroughputController {
    // exponential moving average of measured bytes/s, plus a hard floor
    pub fn advise(&mut self, m: &Metrics) -> Raster { /* cols x rows x fps cap */ }
}
```

---

## 4. Graphics backends

Ordered ladder, probed once at startup and re-probed on resize (cheap, cached):

| # | Backend | Detected by | Raster | Per-frame cost | Notes |
|---|---|---|---|---|---|
| 1 | kitty-shm | `KITTY_WINDOW_ID` + local session | real pixels | 64 B escape | unlink after present |
| 2 | kitty-zlib | kitty graphics probe (`a=q`), Ghostty, WezTerm | real pixels | zlib+base64 | skip zlib when entropy high |
| 3 | iterm2 | `TERM_PROGRAM=iTerm.app`, `LC_TERMINAL` over ssh | real pixels | BMP+base64 | full re-upload |
| 4 | sixel | DA1 reply / `XTSMGRAPHICS` / known `$TERM` | real pixels | palette+dither+RLE | band-parallel |
| 5 | blocks24 | `COLORTERM=truecolor` | 1x2 px/cell | ~37 B/cell | default everywhere |
| 6 | blocks256 | 256-colour terminfo | 1x2 px/cell | ~19 B/cell | 51% of blocks24 (measured) |
| 7 | ascii | always | 1x1 char | lowest | `TERM=dumb` safe |

Rules learned from the spike (`spikes/RESULTS.md`):
- **No delta SGR encoder for truecolor** — measured 0.0-0.1% saving on real content.
- **Indexed mode halves the bytes** — wire it to the throughput controller, not a flag.
- **Compression is content-dependent** — decide per frame from a cheap entropy estimate
  (e.g. zlib ratio of a sample strip, or a histogram of adjacent-pixel deltas).
- **Sixel cost scales with entropy** — reduce palette entries (216 → 64 → 16) on busy frames.
- **tmux/zellij**: detect `$TMUX`/`$ZELLIJ`, require `allow-passthrough`, otherwise drop
  to blocks; document per-multiplexer setup exactly.

---

## 5. Milestones

Each milestone has a **demo** (what you can show), **acceptance** (measurable bar), and
**risk** (what could make it slip). Time estimates assume one focused developer.

### M0 — Skeleton & probe (1 week)

- cargo workspace, CI (Linux/macOS/Windows × stable), `player --version`
- terminal prober: `TIOCGWINSZ`, `CSI 16t`/`14t` cell-pixel query, DA1, env vars, with
  timeouts and caching; a `player probe` subcommand that prints what it found
- `player testsrc` — generates a synthetic pattern (like ffmpeg's `testsrc`) and paints it
  through the selected backend
- tty writer with synchronized-output (`BSU`/`ESU`, DEC 2026) wrapping and resize safety
- throughput harness: measure real `write()` bytes/s to the pty and log it

**Acceptance:** `player testsrc` paints and animates on all seven backends; prober
correctly identifies kitty/WezTerm/Ghostty/iTerm2/foot/Windows Terminal/tmux/dumb; the
harness reports a stable B/s on a `script(1)` pty and over ssh.
**Risk:** pixel-size probing is inconsistent across terminals (answers vary, some hang).
Mitigation: hard 100 ms timeout, fall back to `TIOCGWINSZ` aspect guess, config override.

### M1 — Video core (2 weeks) → *"it plays a video"*

- ffmpeg demux + decode with a bounded frame channel and a decode thread
- swscale to RGB24 (+ SIMD resize); hwdec probe with graceful fallback
- clock: monotonic master (no audio yet), drop/repeat policy, frame pacing
- kitty (shm + zlib) and blocks24/256 backends wired end-to-end
- resize: retarget the scale without draining the cushion
- seek: keyframe-accurate landing, `←/→`, `J/L` ±10s

**Acceptance:** 720p H.264 plays on kitty at 24/30 fps with <1% dropped frames and
correct pacing; 160x50 blocks play at ≥20 fps; `seek` lands within one frame of target
9 out of 10 times; startup to first frame <120 ms on a warm file.
**Risk:** FFmpeg build friction on Windows/macOS. Mitigation: `ffmpeg-sys-the-third`
vendored build in CI from M0 onwards, and a `--no-ffmpeg` audio-only build feature.

### M2 — Audio & sync (2 weeks) → *"it's a real player"*

- symphonia decode → resample → ring buffer → cpal; device enumeration and selection
- audio master clock with output latency compensation and smoothing; ffplay-style
  `compute_target_delay` thresholds
- volume (perceptual), mute, replaygain/`loudnorm` optional, speed (pitch-preserved via
  `rubato`)
- gapless within a playlist (pre-load next track's decoder)
- audio-only mode: no video track → no raster work at all

**Acceptance:** median A/V drift ≤ ±10 ms over a 10-minute clip (measured by a
timestamps-in-audio test fixture, not by eye); audio-only FLAC/Opus/MP3 plays with no
FFmpeg present; CPU <5% for audio-only; seek during playback resyncs within 100 ms.
**Risk:** ALSA/PipeWire latency variance, and cpal device changes mid-playback.
Mitigation: ring buffer with a configurable cushion, device hot-swap handling, and a
`--audio-latency` escape hatch.

### M3 — The TUI (2 weeks) → *the differentiator*

- three-pane model: library tree · queue · now-playing, `h/j/k/l` + `/` fuzzy search
- keymap as data (remappable), vim-ish defaults, `?` help overlay
- `player file.mkv` fast path: one-shot playback with a single-line HUD, no TUI overhead
- `player` bare: opens the library; playback starts in an embedded pane
- state machine that survives: quit while playing → ask pause/stop/background

**Acceptance:** a cold library of 1,000 files scrolls at 60 fps and filters in <50 ms;
every action reachable in ≤2 keystrokes from the main view; `player --help` renders a
usable cheat sheet; snapshot tests on 5 views × 3 terminal widths (80/120/200).
**Risk:** scope explosion (this is where TUIs die). Mitigation: a closed list of views
for 1.0 (library, queue, now-playing, settings, help) and nothing else.

### M4 — Text on screen (1.5 weeks)

- subtitle decode: SRT, WebVTT, ASS subset (positioning, basic styling, `\an` anchor)
- embedded tracks from MKV/MP4; track selection; `s` cycle, `[`/`]` sync offset
- HUD overlay: progress bar, time, volume, backend + fps + dropped (a stats line)
- toast notifications for actions (volume, speed, backend switch)
- overlay composited in *cell space* on top of the raster, so it works on every backend

**Acceptance:** subtitles legible on blocks24 at 80x24 without covering the whole frame;
ASS anchor positions honoured; offset persists per file; HUD repaints only on change
(no flicker), verified by byte-diffing consecutive frames.
**Risk:** ASS is a huge spec. Mitigation: explicitly support a documented subset, and
render unknown tags as plain text rather than dropping the line.

### M5 — Every terminal (1.5 weeks)

- sixel encoder in Rust: fixed 6x7x6 + ordered Bayer, RLE, band-parallel, palette
  reduction by entropy; target <3 ms at 160x50
- iTerm2 inline-image backend (in-memory BMP)
- **ThroughputController**: EMA of measured bytes/s, decode ms, entropy → chooses
  backend/raster/fps; hysteresis so it doesn't oscillate; a `--stats` readout showing
  the decision and why
- tmux/zellij passthrough detection and documented setup
- `player doctor`: prints environment, backends, why each was chosen, and what to fix

**Acceptance:** sixel ≥25 fps at 160x50 on the test fixture; the controller holds ≥24 fps
by dropping resolution when bandwidth is artificially throttled (`tc`/`pv`) and recovers
within 2 s when it is restored; zero visual corruption at every resize; `player doctor`
diagnoses 10 common misconfigurations correctly.
**Risk:** sixel CPU target missed. Mitigation: if the Rust encoder can't hit 3 ms, cap
sixel at 160x50 and prefer blocks above it — a decision the controller already makes.

### M6 — Memory (1 week) → *the moat*

- SQLite schema + migrations, incremental scan (mtime + size, resumable), watch folders
- watch state per file (position, watched%, last seen), "Continue watching"
- playlists (M3U import/export), queue persistence across restarts
- FTS5 search over titles/artists/paths
- thumbnail strip and audio waveform in the library, drawn with the active backend
- metadata: container tags via ffmpeg/symphonia, optional sidecar art

**Acceptance:** rescan of 10,000 files <2 s when nothing changed; killing the process
mid-playback and relaunching resumes within 1 s; search returns in <20 ms; DB <10 MB for
10k files.
**Risk:** scan blocking the UI. Mitigation: scanner on its own thread, batched inserts
in a transaction, UI reads from an in-memory snapshot.

### M7 — Ship (1 week)

- `cargo install`, AUR, `.deb`, Homebrew tap, Windows zip + winget, Termux notes
- CI matrix with the fixture tests; `--version` embeds commit + FFmpeg build info
- man page, `--help` examples, 60-second quickstart, three demo GIFs (one per backend)
- `SECURITY.md`, `CONTRIBUTING.md`, issue templates, `good first issue` list
- 1.0 tag

**Acceptance:** a fresh user on each of 4 platforms reaches first playback in <3 minutes
following only the README; every accepted backend has a demo GIF recorded from a real
terminal.

### Timeline

Weeks 1-3 (M0-M1): something that plays video. Week 4-5 (M2): something that is a player.
Week 6-7 (M3): something people want to keep open. Weeks 8-12: finish, polish, ship.

**Total ≈ 12 weeks** of focused work to a defensible 1.0.

---

## 6. Testing strategy

- **Unit:** every encoder is a pure function `(&Raster, Rect) -> Vec<u8>`; golden-byte
  snapshot tests capture exact escape sequences, so regressions are visible in diffs.
- **Property:** for any raster and any backend, `decode_of(encode(x))` is monotone in
  brightness and preserves aspect ratio; no encoder emits a control sequence it can't
  close.
- **Snapshot TUI:** render each view at 80/120/200 columns against a `TestBackend`.
- **A/V sync harness:** a fixture video whose left channel carries a frame counter; the
  test asserts drift bounds from the recorded clock, not from watching it.
- **Integration:** `expect`-style pty tests that run `player testsrc` under `script(1)`
  and assert the byte stream opens with `BSU` and closes with `ESU`.
- **Fuzz:** subtitle parser and the sixel RLE encoder (both take untrusted input).
- **Manual matrix (documented, not automated):** kitty, WezTerm, Ghostty, iTerm2, foot,
  Konsole, Windows Terminal, tmux, zellij, `TERM=dumb`.

---

## 7. Risk register

| # | Risk | P | Impact | Mitigation |
|---|---|---|---|---|
| R1 | Sixel in Rust misses the 3 ms budget | M | Slower sixel path | Cap sixel to 160x50; controller already prefers blocks above that |
| R2 | FFmpeg linkage makes install painful on Windows/macOS | M | Adoption | Vendored build in CI from M0; prebuilt binaries; audio-only build without FFmpeg |
| R3 | Terminal pixel-size probing is unreliable | H | Wrong raster, blurry or tiny picture | Timeout + fallback + `--cell-size` override; `player doctor` explains |
| R4 | TUI scope creep (M3) | H | Timeline | Closed view list for 1.0; everything else is a labelled follow-up issue |
| R5 | A/V sync complaints that are actually terminal latency | M | Reputation | Ship the `--stats` readout so users can see drift/decisions; measure, don't argue |
| R6 | tmux passthrough friction | H | "Doesn't work" reports | Auto-detect and fall back to blocks; document exact config per multiplexer |
| R7 | A polished competitor ships first | M | Positioning | Win on the library half (M3/M6), which needs product taste, not just cleverness |
| R8 | ffmpeg-the-third maintenance | L | Build breakage | Pin a known-good FFmpeg; keep the decode boundary behind a thin trait so a swap is local |
| R9 | Overfitting the plan to a Python spike | M | Wrong bets | M0 instruments a real pty and real footage; revisit §4 rules with real numbers |

---

## 8. What we deliberately do not build in 1.0

Streaming-service integration · DRM · a GUI window · video editing/trimming ·
transcoding · remote/server mode · plugin system · sync across devices.

Each is a place where a terminal player has died before. They are all follow-ups, and
they all need the library model (M6) to exist first.
