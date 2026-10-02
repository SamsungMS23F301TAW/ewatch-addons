#!/bin/sh
# Build and run the host preview renderer. Writes PNGs into $1 (default
# docs). Needs only clang++ and zlib (both ship with macOS).
#   tools/preview/build.sh [out_dir] [filter]
set -e
cd "$(dirname "$0")/../.."
OUT="${1:-docs}"
mkdir -p "$OUT"
mkdir -p .pio/preview
BIN=".pio/preview/pixelpet_preview"     # build output stays inside the addon
clang++ -std=c++17 -O2 -Wall -Wextra \
  -Isrc/apps/pet -Isrc/core -Itools/preview \
  tools/preview/preview.cpp \
  src/apps/pet/px.cpp src/apps/pet/petart.cpp src/apps/pet/petmodel.cpp \
  src/apps/pet/scenes.cpp src/core/steps.cpp \
  -lz -o "$BIN"
"$BIN" "$OUT" "$2"
