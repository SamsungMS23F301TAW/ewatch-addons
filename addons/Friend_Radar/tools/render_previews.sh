#!/bin/sh
# Render Friend Radar's screens and animations on the host into PNGs.
#   tools/render_previews.sh [outdir] [--frames] [--bench]   (default: docs/previews)
# Compiles the firmware's Arduino-free renderer with clang++ and runs it.
set -e
cd "$(dirname "$0")/.."
OUT="${1:-docs/previews}"
shift 2>/dev/null || true
BIN="${TMPDIR:-/tmp}/friend_radar_preview"
mkdir -p "$OUT"
clang++ -std=gnu++17 -O2 -Wall -Wextra \
  -Isrc/apps/radar/logic -Isrc/apps/radar/gfx -Itools/preview \
  tools/preview/preview.cpp src/apps/radar/logic/*.cpp src/apps/radar/gfx/*.cpp \
  -lz -o "$BIN"
"$BIN" "$OUT" "$@"
