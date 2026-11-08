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
if [ -z "$FF" ] || [ ! -f "$FIX/clip.mp4" ]; then
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

# ---------------------------------------------------------------- 5. hygiene
echo
echo "hygiene"
# the binary must not leak escape sequences onto stderr
if grep -q $'\033' "$TMP/err" 2>/dev/null; then
    fail "escape sequences leaked to stderr"
else
    pass "nothing but text on stderr"
fi

# stdout of a --version must be plain
$PLAYER --version | grep -q $'\033' && fail "--version emitted escapes" \
    || pass "--version is plain text"

echo
echo "=============================================="
printf ' %d checks, %d failed\n' "$CHECKS" "$FAILS"
if [ "$FAILS" -eq 0 ]; then echo " OK"; exit 0; else echo " FAILED"; exit 1; fi
