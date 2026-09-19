#!/usr/bin/env bash
# Build the traced ROM and measure a per-frame timeline for one input trace.
#   tools/frame/run_timeline.sh <trace> [frames]
set -euo pipefail
TRACE="${1:-cruise}"
FRAMES="${2:-180}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
ROOT="${GEARSYSTEM_ROOT:-$PWD/.toolchain/Gearsystem}"
mkdir -p build/frame

python3 tools/frame/gen_traces.py "$TRACE" build/frame_trace.c
rm -f build/main_tilesector_polar_gg.o build/tilesector_polar_motion_gg.o build/frame_trace_gg.o build/gg-tilesector-polar.*
make GBDK_HOME="$GBDK_HOME" POLAR_PROFILE_HOOKS=1 POLAR_LOCAL_PROJECTION=0 \
  POLAR_EXTRA_OBJS=build/frame_trace_gg.o \
  TILESECTOR_FASTFLAGS="-Wf--opt-code-speed -DTSPF_DEFAULT_APPEARANCE=0u -DTSPF_TRACE_INPUT=1" \
  gg-tilesector-polar >/dev/null
cp build/gg-tilesector-polar.gg "build/frame/$TRACE.gg"
cp build/gg-tilesector-polar.noi "build/frame/$TRACE.noi"

if [ ! -x build/frame_timeline ] || [ tools/frame/frame_timeline.cpp -nt build/frame_timeline ]; then
  g++ -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz" \
    tools/frame/frame_timeline.cpp \
    $(ls "$ROOT"/src/*.cpp "$ROOT"/src/audio/*.cpp | grep -v "main\|Gearsystem.cpp") \
    build/coredbg/emu2413.o build/coredbg/miniz.o -lm -o build/frame_timeline
fi
./build/frame_timeline "build/frame/$TRACE.gg" "build/frame/$TRACE.noi" \
  "build/frame/$TRACE.csv" "$FRAMES" 8
