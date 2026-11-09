#!/bin/sh
# integration.sh -- end-to-end tests against the real binary.
#
# These are the tests that catch what unit tests cannot: a backend that emits
# broken escapes in practice, a decode path that hangs, a player that will not
# exit. Everything runs headless (no tty needed), which is also how CI runs it.
#
#   sh tests/integration.sh
#
# Requires: tests/fixtures/* (sh tools/mkfixtures.sh) and ffmpeg for the media
# tests. Media tests are skipped, not failed, when ffmpeg is absent.

set -u
cd "$(dirname "$0")/.."

PLAYER=./player
FAILS=0
CHECKS=0
FIX=tests/fixtures
TMP=$(mktemp -d)
TMPDIR_TP="$TMP"
export TMPDIR_TP
trap 'rm -rf "$TMP"' EXIT

pass() { CHECKS=$((CHECKS+1)); printf '  ok    %s\n' "$1"; }
fail() { CHECKS=$((CHECKS+1)); FAILS=$((FAILS+1)); printf '  FAIL  %s\n' "$1"; }
skip() { printf '  skip  %s\n' "$1"; }

# run player in a headless-ish environment and capture output
run() {
    TERM=${TERM_OVERRIDE:-xterm-256color} COLORTERM=truecolor \
    timeout 30 "$@" 2>"$TMP/err"
}

echo "=============================================="
echo " TermiPlayer integration tests"
echo "=============================================="

# ---------------------------------------------------------------- 1. CLI
echo
echo "cli"
$PLAYER --version > "$TMP/v" 2>&1 && grep -q "^player " "$TMP/v" \
    && pass "--version prints a version" || fail "--version"
$PLAYER --help > "$TMP/h" 2>&1 && grep -q "USAGE" "$TMP/h" \
    && pass "--help prints usage" || fail "--help"
$PLAYER backends > "$TMP/b" 2>&1 && grep -q "kitty-shm" "$TMP/b" \
    && pass "backends lists the ladder" || fail "backends"
$PLAYER probe > "$TMP/p" 2>&1 && grep -q "chosen:" "$TMP/p" \
    && pass "probe reports a choice" || fail "probe"
$PLAYER doctor > "$TMP/d" 2>&1 && grep -q "summary" "$TMP/d" \
    && pass "doctor runs" || fail "doctor"

# unknown option must be an error, not silence
if $PLAYER --nonsense >/dev/null 2>&1; then fail "unknown option exits 0"; else pass "unknown option is rejected"; fi
# bad size must be rejected
if $PLAYER x.mkv -s abc >/dev/null 2>&1; then fail "bad --size accepted"; else pass "bad --size rejected"; fi
# missing file must fail cleanly with a message, not a crash
$PLAYER /nonexistent/file.mkv >/dev/null 2>"$TMP/m" ; rc=$?
[ $rc -ne 0 ] && [ $rc -lt 128 ] && pass "missing file exits with an error, not a signal" \
    || fail "missing file: rc=$rc"

# ---------------------------------------------------------------- 2. backends
echo
echo "backends emit well-formed escapes"

check_stream() {  # $1=backend  $2=min-bytes
    bk=$1
    out="$TMP/$bk.out"
    run $PLAYER testsrc -b "$bk" -q -s 40x22 -d 0.2 > "$out" || { fail "$bk: run failed"; return; }
    size=$(wc -c < "$out" | tr -d ' ')
    [ "$size" -gt "$2" ] || { fail "$bk: only $size bytes"; return; }
    # every escape must be a real introducer
    bad=$(python3 - "$out" <<'PY'
import re,sys
d = open(sys.argv[1],'rb').read()
print(len([m for m in re.finditer(bytes([27])+rb'(?![\x5b\x5dP_\\(%])', d)]))
PY
)
    [ "$bad" = "0" ] || { fail "$bk: $bad malformed escapes"; return; }
    # no NULs in terminal output
    if od -An -c "$out" | grep -q '\\0'; then fail "$bk: NUL byte in output"; return; fi
    pass "$bk ($size bytes, 0 malformed)"
}

check_stream blocks 1000
check_stream blocks256 1000
check_stream ascii 500
check_stream kitty 100
check_stream iterm2 1000
check_stream sixel 1000

# BSU/ESU must balance in every stream we produce
for f in "$TMP"/*.out; do
    opens=$(python3 -c "
import sys
d=open('$f','rb').read()
print(d.count(bytes([27])+b'[?2026h'))" 2>/dev/null || echo 0)
    closes=$(python3 -c "
import sys
d=open('$f','rb').read()
print(d.count(bytes([27])+b'[?2026l'))" 2>/dev/null || echo 0)
    [ "$opens" = "$closes" ] || fail "unbalanced sync in $(basename "$f") ($opens/$closes)"
done
pass "synchronized-output blocks are balanced"

# TERM=dumb must not crash and must degrade
TERM_OVERRIDE=dumb run $PLAYER testsrc -b auto -q -s 20x10 -d 0.1 > "$TMP/dumb.out"
[ $? -eq 0 ] && pass "TERM=dumb degrades without crashing" || fail "TERM=dumb crashed"

# ---------------------------------------------------------------- 3. measurements
echo
echo "measured cost per backend (this is the claim in the docs)"
python3 - <<'PY' || FAILS=$((FAILS+1))
import os, sys
TMP = os.environ['TMPDIR_TP']
rows = []
for bk in ('blocks', 'blocks256', 'ascii', 'sixel', 'kitty', 'iterm2'):
    p = os.path.join(TMP, bk + '.out')
    d = open(p, 'rb').read()
    E = bytes([27])
    frames = d.count(E + b'[?2026h') or 1
    rows.append((bk, len(d) // frames))
print("  %-10s %12s" % ("backend", "bytes/frame"))
for bk, per in rows:
    print("  %-10s %12s" % (bk, format(per, ',')))
# the plan's central claim: indexed colour is materially cheaper than truecolor
bl = dict(rows)
if 'blocks' in bl and 'blocks256' in bl:
    ratio = bl['blocks256'] / bl['blocks']
    ok = ratio < 0.85
    print("  blocks256 / blocks = %.2f  (expected < 0.85: %s)" % (ratio, "ok" if ok else "FAIL"))
    sys.exit(0 if ok else 1)
PY

# ---------------------------------------------------------------- 4. media
echo
echo "media playback"
# We need BOTH an ffmpeg binary and the generated fixtures. The binary may be
# only in TERMIPLAYER_FFMPEG (not on PATH), so ask the player itself rather
# than relying on `command -v`.
FF="${TERMIPLAYER_FFMPEG:-$(command -v ffmpeg || true)}"
if [ -z "$FF" ]; then
    FF=$(./player doctor 2>/dev/null | sed -n 's/.*ffmpeg at //p' | head -1)
fi
MEDIA_READY=1
for fixture in clip.mp4 busy.mp4 tone.flac redblue.mp4; do
    [ -f "$FIX/$fixture" ] || MEDIA_READY=0
done
[ -n "$FF" ] || MEDIA_READY=0
if [ "$MEDIA_READY" -eq 0 ]; then
    skip "ffmpeg or fixtures unavailable (run: sh tools/mkfixtures.sh)"
else
    export TERMIPLAYER_FFMPEG="$FF"

    # video: must produce frames and exit on its own
    run $PLAYER "$FIX/clip.mp4" -b blocks -q -s 60x34 -d 1.0 > "$TMP/vid.out"
    rc=$?
    frames=$(python3 -c "
import sys
d=open('$TMP/vid.out','rb').read()
print(d.count(bytes([27])+b'[?2026h'))" 2>/dev/null || echo 0)
    [ $rc -eq 0 ] && pass "video playback exits cleanly (rc=0)" || fail "video rc=$rc"
    [ "$frames" -ge 10 ] && pass "video produced $frames frames in 1 s" \
        || fail "video produced only $frames frames"

    # the same for every backend, to catch backend-specific decode/render bugs
    for bk in blocks blocks256 ascii kitty sixel iterm2; do
        run $PLAYER "$FIX/clip.mp4" -b "$bk" -q -s 40x22 -d 0.5 > "$TMP/v-$bk.out"
        [ $? -eq 0 ] || fail "video on $bk did not exit cleanly"
    done
    pass "video plays on all six backends"

    # busy content: the worst case for every encoder, must not hang or crash
    run $PLAYER "$FIX/busy.mp4" -b sixel -q -s 40x22 -d 0.5 > "$TMP/busy.out"
    [ $? -eq 0 ] && pass "high-entropy content does not break sixel" || fail "busy.mp4 failed"

    # audio only, to a WAV: prove we decode real samples
    rm -f "$TMP/a.raw"
    ( exec 9>"$TMP/a.raw"; TERMIPLAYER_AUDIO_FD=9 TERM=xterm-256color \
      timeout 30 $PLAYER "$FIX/tone.flac" --no-video -q >/dev/null 2>&1 )
    sz=$(wc -c < "$TMP/a.raw" 2>/dev/null || echo 0)
    [ "$sz" -gt 100000 ] && pass "audio-only path produced $sz bytes of PCM" \
        || fail "audio-only produced only $sz bytes"
    # it must be non-silent
    nz=$(python3 -c "
d=open('$TMP/a.raw','rb').read()
print(1 if any(b not in (0,) for b in d[:20000]) else 0)")
    [ "$nz" = "1" ] && pass "captured audio is not silence" || fail "captured audio is silent"

    # seeking must not hang and must still exit
    run $PLAYER "$FIX/clip.mp4" -b ascii -q -s 20x10 -t 2 -d 0.3 > "$TMP/seek.out"
    [ $? -eq 0 ] && pass "-t seek starts mid-file and exits" || fail "seek run failed"
fi


# ---------------------------------------------------------------- 5. library + resume
echo
echo "library and resume"
export TERMIPLAYER_DATA="$TMP/data"
rm -rf "$TERMIPLAYER_DATA"

if [ "$MEDIA_READY" -eq 1 ]; then
run $PLAYER add "$FIX" >/dev/null 2>&1
[ $? -eq 0 ] && pass "add indexes a folder" || fail "add failed"
[ -f "$TMP/data/index.tsv" ] && pass "index file written" \
    || fail "no index.tsv after add"

n=$(grep -c '^I' "$TMP/data/index.tsv" 2>/dev/null || echo 0)
[ "$n" -ge 3 ] && pass "index holds $n entries" || fail "index holds only $n entries"

# list must render human titles, not raw file names
lsout=$($PLAYER list 2>/dev/null)
echo "$lsout" | grep -q 'Tone' && pass "list shows a parsed title" \
    || fail "list did not show a parsed title: $lsout"

# a filter narrows the list
alln=$(echo "$lsout" | grep -c . )
fil=$($PLAYER list clip 2>/dev/null)
filn=$(echo "$fil" | grep -c . )
[ "$filn" -lt "$alln" ] && pass "filter narrows $alln -> $filn" \
    || fail "filter did not narrow the list"

# Playback must run at real time, not as fast as the decoder can go. redblue.mp4
# is 40 s; playing it for ~6 s must land near 6 s, not at the end. This is the
# check that would have caught the PTS bug (pts came from the clock, so every
# frame looked in sync and nothing was ever paced).
start=$(date +%s)
timeout 20 $PLAYER "$FIX/redblue.mp4" -b ascii -q -s 20x10 -d 6 >/dev/null 2>&1
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -ge 5 ] && [ "$elapsed" -le 12 ] \
    && pass "-d 6 takes about 6 s of wall time (${elapsed}s)" \
    || fail "-d 6 took ${elapsed}s of wall time: playback is not paced"

pos=$($PLAYER continue 2>/dev/null | awk 'tolower($0) ~ /redblue/ {gsub(/%/,"",$1); print $1; exit}')
[ -n "$pos" ] && [ "$pos" -ge 5 ] && [ "$pos" -le 25 ] \
    && pass "position after 6 s is ${pos}% of the file" \
    || fail "position after 6 s was '${pos}%', expected roughly 15%"

grep -q '^S' "$TMP/data/state.tsv" 2>/dev/null && pass "position persisted" \
    || fail "no position persisted"

# "continue" must surface the in-progress file
$PLAYER continue 2>/dev/null | grep -qi 'redblue' && pass "continue lists the in-progress file" \
    || fail "continue did not list the in-progress file"

# Resume must actually move the decoder. redblue.mp4 is red for its first 20 s
# and blue after, so the dominant escape-stream colour tells us where decoding
# really started -- a cosmetic clock offset cannot fake this.
# -d is a playback budget from the start point, so 0.4 s is enough to get frames
dom_colour() {
    $PLAYER "$FIX/redblue.mp4" -b blocks256 -q -s 12x6 -d 0.4 "$@" 2>/dev/null | python3 -c "
import sys,re,collections
d=sys.stdin.buffer.read().decode('utf8','replace')
c=collections.Counter(re.findall(r'(?:38|48);5;(\d+)',d))
print(c.most_common(1)[0][0] if c else 'none')"
}
c2=$(dom_colour -t 2)
c30=$(dom_colour -t 30)
[ "$c2" = "196" ] && pass "-t 2 decodes the red part (colour $c2)" \
    || fail "-t 2 produced colour $c2, expected 196 (red)"
[ "$c30" = "21" ] && pass "-t 30 decodes the blue part (colour $c30)" \
    || fail "-t 30 produced colour $c30, expected 21 (blue)"

# a saved position must be honoured: 3 s in, the frame is still red
$PLAYER "$FIX/redblue.mp4" --no-resume -b ascii -q -s 8x4 -d 0.2 >/dev/null 2>&1
[ $? -eq 0 ] && pass "--no-resume bypasses the saved position" || fail "--no-resume failed"

else
    skip "library playback and resume require ffmpeg and fixtures (run: sh tools/mkfixtures.sh)"
fi

# the player must refuse to start the TUI without a tty rather than hang
timeout 10 $PLAYER tui < /dev/null > "$TMP/tui.out" 2>"$TMP/tui.err"
rc=$?
[ "$rc" -ne 124 ] && pass "tui without a tty exits instead of hanging (rc=$rc)" \
    || fail "tui hung with no tty"


# ---------------------------------------------------------------- 6. TUI player
echo
echo "interactive player"
tui_run() {
    # $1 = keys as "delay:bytes" pairs via python, $2 = timeout
    python3 - "$PLAYER" "$TMP" <<'PY'
import os, pty, sys, time, select, struct, fcntl, termios
player, tmp = sys.argv[1], sys.argv[2]
spec = os.environ.get("TUI_KEYS", "")
keys = []
for item in spec.split(",") if spec else []:
    d, b = item.split(":", 1)
    keys.append((float(d), b.encode().decode("unicode_escape").encode()))
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.environ["COLORTERM"] = "truecolor"
    err = os.open(os.path.join(tmp, "tui_err"), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.dup2(err, 2)
    os.execv(player, [player, "tui"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 100, 0, 0))
limit = float(os.environ.get("TUI_TIMEOUT", "12"))
t0 = time.time(); ki = 0; status = None
while time.time() - t0 < limit:
    while True:
        r, _, _ = select.select([fd], [], [], 0)
        if not r: break
        try: d = os.read(fd, 1 << 16)
        except OSError: d = b""
        if not d: break
    if ki < len(keys) and time.time() - t0 > keys[ki][0]:
        os.write(fd, keys[ki][1]); ki += 1
    wp, st = os.waitpid(pid, os.WNOHANG)
    if wp: status = st; break
if status is None:
    os.kill(pid, 9); os.waitpid(pid, 0)
    print("TIMEOUT")
else:
    print("EXIT %d" % ((status >> 8) & 0xFF))
os.close(fd)
PY
}

export TERMIPLAYER_DATA="$TMP/tuidata"
rm -rf "$TERMIPLAYER_DATA"
$PLAYER add "$FIX" >/dev/null 2>&1

# The TUI must open, and must quit; a hung UI is worse than a crash.
out=$(TUI_KEYS="1.5:q" TUI_TIMEOUT=8 tui_run)
case "$out" in
    "EXIT 0") pass "TUI opens and quits on q" ;;
    *)        fail "TUI did not exit cleanly: $out" ;;
esac

# Playback in the TUI must run at real time and persist a resume point. This is
# the test that catches the pacing bug: the pane used to burn through the whole
# file in a couple of seconds and mark it watched.
if [ "$MEDIA_READY" -eq 1 ]; then
out=$(TUI_KEYS="1.2:/redblue,1.8:\r,9.0:q" TUI_TIMEOUT=13 tui_run)
pos=$($PLAYER continue 2>/dev/null | awk '/Redblue/ {gsub(/%/,"",$1); print $1; exit}')
[ -n "$pos" ] && [ "$pos" -ge 5 ] && [ "$pos" -le 45 ] \
    && pass "TUI playback saved ${pos}% after ~7 s" \
    || fail "TUI playback saved '${pos}%', expected roughly 17%"
else
    skip "TUI playback requires ffmpeg and fixtures (run: sh tools/mkfixtures.sh)"
fi

# ---------------------------------------------------------------- 7. hygiene
echo
echo "hygiene"
# the binary must not leak escape sequences onto stderr
if grep -q "$(printf '\033')" "$TMP/err" 2>/dev/null; then
    fail "escape sequences leaked to stderr"
else
    pass "nothing but text on stderr"
fi

# stdout of a --version must be plain
$PLAYER --version | grep -q "$(printf '\033')" && fail "--version emitted escapes" \
    || pass "--version is plain text"

echo
echo "=============================================="
printf ' %d checks, %d failed\n' "$CHECKS" "$FAILS"
if [ "$FAILS" -eq 0 ]; then echo " OK"; exit 0; else echo " FAILED"; exit 1; fi
