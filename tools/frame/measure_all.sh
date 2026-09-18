#!/usr/bin/env bash
# Timeline every trace with whatever renderer is currently in src/.
set -euo pipefail
TAG="${1:-cur}"
FRAMES="${2:-100}"
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
  ./build/frame_timeline "build/frame/$T.gg" "build/frame/$T.noi" \
    "build/frame/$T-$TAG.csv" "$FRAMES" 8 > "build/frame/tl-$T-$TAG.txt" 2>/dev/null
  printf '%-9s %-4s mean %9s  p50 %9s  p95 %9s  mat %9s\n' "$T" "$TAG" \
    "$(grep -m1 '^    mean' build/frame/tl-$T-$TAG.txt | awk '{print $2}')" \
    "$(grep -m1 '^    mean' build/frame/tl-$T-$TAG.txt | awk '{print $4}')" \
    "$(grep -m1 '^    mean' build/frame/tl-$T-$TAG.txt | awk '{print $6}')" \
    "$(grep -m1 'materializer' build/frame/tl-$T-$TAG.txt | awk '{print $2}')"
done
