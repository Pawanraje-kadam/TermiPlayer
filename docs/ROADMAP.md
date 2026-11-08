# Roadmap after M2

Current state: **M0–M2 done and verified** (see [`BUILD-LOG.md`](BUILD-LOG.md)).
Video and audio play, seven backends work, the clock is correct, and the
throughput controller makes real decisions.

What follows is ordered by two criteria: what is *unverified*, first; then what
unlocks the most value per unit of work.

---

## 1. Verify what this environment could not (do this first)

Everything here is implemented but **unproven**, because the build sandbox has no
tty and no audio device. Each item has a concrete way to check it.

| Task | How to verify | Why it matters |
|---|---|---|
| `CSI 16t`/`14t` cell-pixel probing | run `player probe` in kitty, WezTerm, Ghostty, foot, iTerm2, Windows Terminal; compare reported cell size to the real one | if it is wrong, every pixel backend renders at the wrong scale |
| kitty graphics (shm and zlib) | play a clip in kitty locally, then over ssh | the shm path places an object by name; the zlib path is the only thing making remote kitty usable |
| sixel correctness | play in foot/contour/mlterm; check orientation and palette | sixel is stateful and easy to get subtly wrong (bands, `$` vs `-`) |
| iTerm2 OSC 1337 | play in iTerm2 and WezTerm | chunking and raw-RGB `size=` semantics |
| tmux / zellij fallback | play inside tmux with and without `allow-passthrough` | the pessimistic ladder must not show garbage |
| audio on a real device | play an album on PipeWire and on plain ALSA | the `pa_simple` path is untested; latency may need tuning |
| sustained fps | `player FILE --stats` for 10 minutes per backend | the only number users actually judge |
| A/V drift over time | the counter-in-audio fixture (`tests/fixtures` needs one) | drift is invisible in 10 seconds and maddening in 90 minutes |
| resize during playback | drag a window mid-playback on every backend | stale raster and SIGWINCH races live here |
| hwdec benefit | `ffmpeg -hwaccel auto` vs `none` on a 4K file | decides whether exposing `-hwaccel` is worth it |

**Exit criterion:** a documented matrix in `docs/COMPATIBILITY.md` with a row per
terminal, filled in from real runs, including the ones that fail.

---

## 2. Port to Rust (the original plan)

The C implementation is a working specification. Porting it is mechanical if done
in this order, because each step is independently testable:

1. `raster` + `util` + base64 — pure functions, property-testable
2. `gfx` backends — golden-byte snapshot tests against the C output
3. `term` — probe and `TtyWriter`
4. `decode` — **at this point switch `DecodeSource` for `ffmpeg-the-third`** and
   delete the subprocess. This is the payoff: one fewer copy per frame and no
   runtime ffmpeg dependency.
5. `audio` — `symphonia` + `cpal`, replacing both the subprocess audio *and* the
   dlopen'd pulse sink. Then audio-only playback genuinely needs nothing.
6. `clock`, `throughput` — port the tests first, they encode the policy
7. `session`, then `main`
8. M3: `ratatui` views

Keep `tests/integration.sh` exactly as it is: it tests the binary's observable
behaviour, so it is the acceptance suite for the port. If it passes against the
Rust binary, the port is correct.

**Do not** port and add features at the same time.

---

## 3. M3 — the TUI (the differentiator)

Nothing about the current core needs to change to support this; `session_run()`
is already the thing a view calls into.

- three panes: library tree / queue / now-playing
- keymap as data, vim-ish defaults, `?` overlay
- fuzzy filter over 1,000s of files in <50 ms
- playback embedded in a pane, with the HUD as a widget rather than raw escapes
- state machine for quit-while-playing (pause / stop / background)
- snapshot tests: 5 views x 3 terminal widths

Requires §4 first, because a library view with no library is a mockup.

---

## 4. M6 — the library (the moat)

- SQLite via `rusqlite` (bundled) or, in C, a single-file append-only index
- incremental scan keyed on (mtime, size), resumable, on its own thread
- watch state: position, percent, last seen; "Continue watching"
- playlists, M3U import/export
- FTS5 search over titles, artists, paths
- thumbnails and waveforms drawn with the *active* backend

This is the half of the product nobody else has. It needs taste, not cleverness —
which is why it is worth doing after the engine is proven rather than before.

---

## 5. M4 — text on screen

- SRT, WebVTT, ASS subset (positioning, styling, `\an` anchors)
- embedded tracks from MKV/MP4, track selection, `[`/`]` sync offset
- overlay composited in cell space so it works on every backend
- toasts, chapter markers, notifications

---

## 6. Known rough edges in the current code

Listed so they are not mistaken for finished work.

| Where | Issue |
|---|---|
| `session.c` seek | re-opens the decoder instead of seeking in place; works, but re-spawns ffmpeg and loses the buffer. Fix with `-ss` on the child's argv. |
| `session.c` speed | `clock_set_speed` changes pacing but does not resample audio, so audio and video drift at speeds != 1.0. Needs pitch-preserved resampling (M2 remainder). |
| `gfx_kitty.c` shm | the shm object is unlinked by the *terminal*; if it never maps it, we leak one object per frame. Bound it with a small pool and a timeout. |
| `gfx_sixel.c` | quantisation is scalar. A band-parallel + SIMD version is where the 3 ms target has to come from. |
| `audio.c` | `pa_simple` has no flush; a seek costs one buffer of stale audio. A raw `pa_stream` would allow a real flush. |
| `decode.c` | `probe_media` parses ffmpeg's human-readable banner. Fragile across versions; use `-f ffmetadata` or a real ffprobe when available. |
| `decode.c` | `-hwaccel` is not exposed. Add a `--hwdec auto\|none` flag. |
| `throughput.c` | the entropy estimate samples a fixed grid; on very wide rasters it under-samples. Stride by area, not by a constant. |
| `session.c` | two decoders means seeking can land audio and video on slightly different frames. One process with two outputs would be exact. |
| everywhere | no `--hwdec`, no subtitle rendering, no chapters, no playlist, no resume. |

---

## 7. Packaging (M7, can start early)

- `make install` exists; add AUR, `.deb`, Homebrew, winget, Termux notes
- **document the ffmpeg runtime dependency prominently** — it is the single most
  likely "it doesn't work" report
- ship a `player doctor` that already answers the top five support questions
- demo GIFs per backend, recorded from real terminals, in the README

---

## 8. Non-goals, restated

Streaming services, DRM, a GUI window, editing, transcoding, device sync,
plugins. Each is a place a terminal player has died. The library model has to
exist first, and it does not yet.
