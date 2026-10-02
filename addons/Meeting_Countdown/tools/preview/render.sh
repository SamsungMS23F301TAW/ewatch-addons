#!/bin/sh
# Render every Meeting Countdown screen on the host with the watch's own
# drawing code, then convert to PNG.
#   sh tools/preview/render.sh [outdir]      (default: .scratch/preview)
# Needs clang++ (or c++) and python3 with Pillow.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-.scratch/preview}
mkdir -p "$OUT"
CXX=${CXX:-c++}
$CXX -std=c++17 -O2 -Wall -Wextra -Itools/hostinc -Isrc/apps/assets/fonts -Ilib/meetcore/src \
  tools/preview/render_previews.cpp lib/meetcore/src/*.cpp -o "$OUT/render_previews"
"$OUT/render_previews" "$OUT"
python3 tools/preview/to_png.py "$OUT"
