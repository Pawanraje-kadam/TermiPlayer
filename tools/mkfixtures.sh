#!/bin/sh
# Generate the test media. Requires ffmpeg on PATH.
#
# The fixtures are deliberately small and cover the cases that break players:
#   clip.mp4    moving pattern + 440 Hz tone, 4 s -- sync and pacing
#   busy.mp4    high-entropy noise, 2 s            -- worst case for every encoder
#   flat.mp4    static gradient, 2 s               -- best case (run-length)
#   redblue.mp4 40 s, red for 20 s then blue       -- proves a seek moved
#   tone.flac   5 s 440 Hz, audio only             -- the audio-only path
set -e
FF="${TERMIPLAYER_FFMPEG:-ffmpeg}"
cd "$(dirname "$0")/.."
mkdir -p tests/fixtures
$FF -y -hide_banner -loglevel error \
    -f lavfi -i "testsrc2=size=320x180:rate=24:duration=4" \
    -f lavfi -i "sine=frequency=440:duration=4" \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest tests/fixtures/clip.mp4
# high-entropy content: noise has no size/rate of its own, so build it on top
# of a scaled source rather than passing unsupported option
$FF -y -hide_banner -loglevel error \
    -f lavfi -i "nullsrc=size=320x180:rate=24:duration=2,noise=alls=100:allf=t+u" \
    -c:v libx264 -pix_fmt yuv420p tests/fixtures/busy.mp4
$FF -y -hide_banner -loglevel error \
    -f lavfi -i "gradients=size=320x180:rate=24:duration=2" \
    -c:v libx264 -pix_fmt yuv420p tests/fixtures/flat.mp4
# A seek is only visibly correct if the frame content changes with time. This
# clip is red for its first half and blue for its second: decoding at 2 s must
# yield red and at 30 s blue, which a purely cosmetic clock cannot fake.
$FF -y -hide_banner -loglevel error \
    -f lavfi -i "color=c=red:size=160x90:rate=24:duration=20" \
    -f lavfi -i "color=c=blue:size=160x90:rate=24:duration=20" \
    -filter_complex "[0:v][1:v]concat=n=2:v=1:a=0" \
    -c:v libx264 -pix_fmt yuv420p tests/fixtures/redblue.mp4
$FF -y -hide_banner -loglevel error \
    -f lavfi -i "sine=frequency=440:duration=5" -c:a flac tests/fixtures/tone.flac
ls -la tests/fixtures/
