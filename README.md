# TermiPlayer (`player`)

**A media player that lives inside your terminal.** Video *and* audio, offline, keyboard-first,
one binary — with a real library, a queue, and resume-where-you-left-off.

```bash
player movie.mkv            # just play the thing
player                      # open the library
player --list               # what did I watch, and where did I stop
```

> **Status: research & planning (M0 not started).**
> This repository currently contains the market research, a measured feasibility
> spike, and the full build plan. See [`docs/RESEARCH-AND-PLAN.md`](docs/RESEARCH-AND-PLAN.md).

---

## The one-sentence pitch

> Everyone else built a *renderer*. We are building the first **player**.

That distinction is the entire project. `mpv --vo=tct`, `ncplayer`, `timg`, `chafa` and the
recent wave of Rust terminal players all answer "how do I get pixels into a terminal?".
None of them answer "I have 400 files, what do I watch next, and put me back where I stopped."

---

## What it looks like

```
┌ TermiPlayer ────────────────────────────── ── ♪ 03:12 / 1:47:38 ── Volume 72% ─┐
│ LIBRARY › Films › Sci-Fi (18)                     │  BLADE RUNNER 2049           │
│                                                    │  Denis Villeneuve · 2017      │
│  ▸ Arrival                          2016   ✓ 87%  │  2h 43m · 4K HDR · HEVC      │
│  ▸ Blade Runner 2049                2017   ✓ 100% │                              │
│  ▸ Dune                             2021   ✓ 41%  │  ┌──────────────────────────┐│
│  ▸ Dune: Part Two                   2024       —  │  │                          ││
│  ▸ Ex Machina                       2014   ✓ 100% │  │      video renders        ││
│  ▸ Interstellar                     2014   ✓ 12%  │  │      here at the          ││
│                                                    │  │      terminal's real      ││
│  QUEUE                                             │  │      pixel resolution     ││
│  1. Dune: Part Two          ← next                 │  │                          ││
│  2. Annihilation                                   │  └──────────────────────────┘│
│  3. Sicario                                        │                              │
├────────────────────────────────────────────────────┴──────────────────────────────┤
│ ▁▂▃▅▇█▇▅▃▂▁  space pause · ←→ seek · j/k vol · s subs · f fullscreen · q quit     │
└───────────────────────────────────────────────────────────────────────────────────┘
```

*Renders at whatever your terminal can do: real pixels (kitty/WezTerm/Ghostty/iTerm2),
sixel, or truecolor blocks — auto-detected, over ssh, inside tmux.*

---

## Why it will stand out

| | Today | TermiPlayer |
|---|---|---|
| **Scope** | renderer, or library, never both | renderer **and** library **and** queue **and** resume |
| **Audio** | `ncplayer`, `timg`, `chafa` have none | audio is a first-class citizen, zero external deps |
| **Dependencies** | needs mpv + yt-dlp installed | one binary; pure-Rust audio path |
| **Slow links** | fixed quality, stutters | **adaptive throughput controller** — measures your pty and renegotiates resolution/fps live |
| **Resume** | none of them remember anything | SQLite watch-state, per-file position, played markers |
| **Fidelity** | folklore-driven (delta-encoding SGR) | **measured** — our spike shows that trick saves 0.0%, so we don't do it |

Full analysis, with star counts, source-verified claims and the measured byte budget:
**[`docs/COMPETITORS.md`](docs/COMPETITORS.md)**.

---

## Evidence, not vibes

The plan rests on a spike in [`spikes/`](spikes/) that measured bytes-per-frame and
encode cost for six backends across three content types and three terminal sizes.

The headline result contradicts what the whole niche repeats:

```
            24-bit   delta%   sixel      kitty   kitty+z
80x24        71,301      0.1   4,228     15,504     7,852   bytes/frame
320x90    1,067,412      0.0  15,145    232,566    52,114   bytes/frame
```

Delta-encoding SGR pair by pair — the trick `rtv` claims buys "30-50% fewer bytes" —
saves **0.0%** on real footage, because every cell in truecolor video is already a
unique colour. What actually matters is a hard ceiling no one talks about:

> **At 320x90, 24-bit half-blocks max out at 9 fps on a 10 MB/s pty.**
> The design must negotiate resolution against frame rate, not pretend it can have both.

That constraint is what the adaptive throughput controller exists to solve, and it is
the mechanism nobody else in this space has.

Numbers, method and the full results table: [`spikes/RESULTS.md`](spikes/RESULTS.md).

---

## Planned features

**Playback**
- Video: anything FFmpeg decodes (H.264, HEVC, AV1, VP9, …), hardware decode where available
- Audio: MP3, FLAC, Opus, Vorbis, AAC, WAV, ALAC — **pure Rust, no system dependencies**
- Correct A/V sync (audio master clock, ffplay-compatible thresholds)
- Gapless audio, gapless playlists, pitch-preserving speed control
- Seeking with keyframe-accurate landing, frame step, chapter jump

**Terminal output** — auto-detected ladder
- kitty graphics (shared-memory when local, zlib when remote)
- iTerm2 inline images
- sixel (tuned fixed palette + ordered dither + RLE)
- truecolor half-blocks, 256-colour blocks, ASCII
- **Adaptive throughput controller**: measures real `write()` throughput and trades
  resolution for frame rate to keep audio and video locked on slow links
- tmux/zellij passthrough detection with automatic fallback

**The player part** (what nobody else has)
- Library index over watched folders, SQLite, incremental rescan
- Resume per file; "Continue watching" row; played/unplayed markers
- Queue, playlists (M3U import/export), shuffle, repeat
- Subtitles: embedded + external SRT/VTT/ASS-subset, styling, sync offset
- Watch-later, ratings, filters, fuzzy search
- Thumbnails and waveforms in the library (drawn with the same graphics backend)
- Themes, full keybinding remap, config file, shell completions, man page

**Out of scope for 1.0** (deliberately): streaming services, DRM, GUI, editing.

---

## Plan

Seven milestones to a shippable 1.0, roughly 12 weeks of focused work.
Each one is independently usable — there is never a broken intermediate state.

| | Milestone | Ships | Weeks |
|---|---|---|---|
| **M0** | Skeleton & probe | `player testsrc` paints a pattern; backend ladder auto-detects | 1 |
| **M1** | Video core | real video plays, video-only, kitty + blocks | 2 |
| **M2** | Audio & sync | A/V locked, volume, seek, speed | 2 |
| **M3** | The TUI | library, queue, now-playing, keys — *the differentiator* | 2 |
| **M4** | Text on screen | subtitles, chapters, HUD, notifications | 1.5 |
| **M5** | Every terminal | sixel, iTerm2, adaptive controller, tmux | 1.5 |
| **M6** | Memory | SQLite index, resume, playlists, thumbnails | 1 |
| **M7** | Ship | packaging, CI matrix, docs, demo GIFs, 1.0 | 1 |

Detailed scope, acceptance criteria and risk register:
**[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** and the roadmap section of
**[`docs/RESEARCH-AND-PLAN.md`](docs/RESEARCH-AND-PLAN.md)**.

---

## Tech stack

Rust (stable). `ratatui` + `crossterm` for the TUI, `ffmpeg-the-third` for video decode,
`symphonia` for pure-Rust audio decode, `cpal` for output, `rusqlite` for the index,
hand-written encoders for every graphics backend (no libsixel, no notcurses).

---

## License

MIT — see [LICENSE](LICENSE).
