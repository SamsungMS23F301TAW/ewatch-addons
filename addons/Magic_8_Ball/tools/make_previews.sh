#!/bin/sh
# Rebuild the host preview renderer and refresh the screenshots in docs/.
#
#   tools/make_previews.sh            # writes docs/*.png (+ docs/oracle.gif)
#   tools/make_previews.sh out_dir    # somewhere else
#
# Needs clang++ (or g++) and zlib. The filmstrip and GIF also need Python 3
# with Pillow; without it only the PNG screenshots are written.
set -eu
cd "$(dirname "$0")/.."
OUT="${1:-docs}"
CXX="${CXX:-clang++}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$OUT"
"$CXX" -std=c++17 -O2 -Wall -Wextra -DPROGMEM= \
  -Itools/host_include -Itools -Isrc/apps/assets/fonts -Isrc/apps/oracle/core \
  tools/oracle_preview.cpp src/apps/oracle/core/*.cpp -lz -o "$TMP/oracle_preview"

"$TMP/oracle_preview" "$OUT"

if python3 -c "import PIL" 2>/dev/null; then
  mkdir -p "$TMP/frames"
  "$TMP/oracle_preview" "$TMP/frames" --frames > /dev/null
  python3 tools/compose_docs.py "$TMP/frames" "$OUT"
else
  echo "Pillow not found: skipped docs/sequence.png and docs/oracle.gif"
fi
