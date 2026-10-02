#!/bin/sh
# Build the host tools (bench, replay, preview renderer) into build/host/.
# Needs only clang++ (or g++) with C++17.
set -e
cd "$(dirname "$0")/../.."
CXX=${CXX:-clang++}
OUT=build/host
mkdir -p "$OUT"
FLAGS="-std=c++17 -O2 -Wall -Wextra -Isrc/apps/reps -Itest/support"
CORE="src/apps/reps/rep_detector.cpp"
$CXX $FLAGS tools/host/rep_bench.cpp $CORE -o "$OUT/rep_bench"
$CXX $FLAGS tools/host/rep_replay.cpp $CORE -o "$OUT/rep_replay"
$CXX $FLAGS tools/host/rep_synth_csv.cpp $CORE -o "$OUT/rep_synth_csv"
[ -f tools/host/rep_preview.cpp ] && $CXX $FLAGS -Isrc/apps/assets/fonts tools/host/rep_preview.cpp \
    src/apps/reps/rep_ui.cpp src/apps/reps/rep_gfx.cpp src/apps/reps/rep_session.cpp -o "$OUT/rep_preview"
echo "built: $(ls $OUT | tr '\n' ' ')"
