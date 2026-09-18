#!/usr/bin/env bash
# Census the materializer workload on every trace.
set -euo pipefail
FRAMES="${1:-100}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
mkdir -p build/frame
for T in cruise spin corners stress; do
  python3 tools/frame/gen_traces.py "$T" build/frame_trace.c >/dev/null
  rm -f build/main_tilesector_polar_gg.o build/frame_trace_gg.o build/gg-tilesector-polar.*
  make GBDK_HOME="$GBDK_HOME" POLAR_PROFILE_HOOKS=1 POLAR_LOCAL_PROJECTION=0 \
    POLAR_EXTRA_OBJS=build/frame_trace_gg.o \
    TILESECTOR_FASTFLAGS="-Wf--opt-code-speed -DTSPF_DEFAULT_APPEARANCE=0u -DTSPF_TRACE_INPUT=1" \
    gg-tilesector-polar >/dev/null
  cp build/gg-tilesector-polar.gg "build/frame/$T.gg"
  cp build/gg-tilesector-polar.noi "build/frame/$T.noi"
  ./build/mat_census "build/frame/$T.gg" "build/frame/$T.noi" "$T" "$FRAMES" 8 \
    | tee "build/frame/mat-$T.txt"
  echo
done
