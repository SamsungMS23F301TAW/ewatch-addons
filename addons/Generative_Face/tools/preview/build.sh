#!/bin/sh
# Builds the host preview renderer from the same sources the watch uses.
set -e
cd "$(dirname "$0")"
ROOT=../..
FACE=""
[ -f "$ROOT/src/art/gf_face.cpp" ] && FACE="-DGF_HAVE_FACE"
clang++ -std=c++17 -O2 -Wall -Wextra $FACE -I"$ROOT/src/art" \
  preview.cpp "$ROOT"/src/art/*.cpp -lz -o preview
echo "built tools/preview/preview"
