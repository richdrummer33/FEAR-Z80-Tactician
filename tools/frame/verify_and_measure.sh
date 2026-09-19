#!/usr/bin/env bash
# Rebuild every trace against the current renderer, prove the name table is
# byte-identical to the captured baseline, and only then report timings.
set -euo pipefail
FRAMES="${1:-60}"
OUT="${2:-build/frame/new}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
mkdir -p "$OUT"
FAIL=0
for T in cruise spin corners stress; do
  python3 tools/frame/gen_traces.py "$T" build/frame_trace.c >/dev/null
  rm -f build/main_tilesector_polar_gg.o build/tilesector_polar_motion_gg.o build/frame_trace_gg.o build/gg-tilesector-polar.*
  make GBDK_HOME="$GBDK_HOME" POLAR_PROFILE_HOOKS=1 POLAR_LOCAL_PROJECTION=0 \
    POLAR_EXTRA_OBJS=build/frame_trace_gg.o \
    TILESECTOR_FASTFLAGS="-Wf--opt-code-speed -DTSPF_DEFAULT_APPEARANCE=0u -DTSPF_TRACE_INPUT=1" \
    gg-tilesector-polar >/dev/null
  cp build/gg-tilesector-polar.gg "build/frame/$T.gg"
  cp build/gg-tilesector-polar.noi "build/frame/$T.noi"
  ./build/mat_census "build/frame/$T.gg" "build/frame/$T.noi" "$T" "$FRAMES" 8 "$OUT/$T.hash" \
    > "$OUT/mat-$T.txt" 2>/dev/null
  if diff -q "build/frame/base/$T.hash" "$OUT/$T.hash" >/dev/null; then
    echo "  $T: name table IDENTICAL over $FRAMES frames"
  else
    echo "  $T: NAME TABLE DIFFERS -- $(diff "build/frame/base/$T.hash" "$OUT/$T.hash" | grep -c '^<') frames"
    FAIL=1
  fi
done
if [ "$FAIL" != 0 ]; then
  echo "OUTPUT EQUIVALENCE FAILED; timings withheld"
  exit 1
fi
echo "OUTPUT EQUIVALENCE OK"
