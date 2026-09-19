#!/usr/bin/env bash
# How much does turning faster cost, in whole-loop T-states?
#
# Turn rate and frame rate are coupled: a faster turn moves more boundaries an
# update, so the renderer does more work, so the update takes longer. This
# rebuilds the traced ROM at several turn rates and times the same trace on each.
set -euo pipefail
TRACE="${1:-spin}"
FRAMES="${2:-100}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
mkdir -p build/frame
python3 tools/frame/gen_traces.py "$TRACE" build/frame_trace.c >/dev/null
printf '%-6s %-9s %10s %10s %10s %10s\n' "turnQ4" "deg/s@20" "mean" "p50" "p95" "worst"
for Q in ${QLIST:-48 64 144}; do
  DPS=$(python3 -c "print(f'{(($Q+8)>>4)*360/256*20:.0f}')")
  rm -f build/main_tilesector_polar_gg.o build/tilesector_polar_motion_gg.o \
        build/frame_trace_gg.o build/gg-tilesector-polar.*
  make GBDK_HOME="$GBDK_HOME" POLAR_PROFILE_HOOKS=1 POLAR_LOCAL_PROJECTION=0 \
    POLAR_EXTRA_OBJS=build/frame_trace_gg.o \
    TILESECTOR_FASTFLAGS="-Wf--opt-code-speed -DTSPF_DEFAULT_APPEARANCE=0u -DTSPF_TRACE_INPUT=1 -DMANUAL_TURN_Q4=$Q" \
    gg-tilesector-polar >/dev/null
  cp build/gg-tilesector-polar.gg "build/frame/turn$Q.gg"
  cp build/gg-tilesector-polar.noi "build/frame/turn$Q.noi"
  ./build/frame_timeline "build/frame/turn$Q.gg" "build/frame/turn$Q.noi" \
    "build/frame/turn$Q.csv" "$FRAMES" 8 > "build/frame/turn$Q.txt" 2>/dev/null
  L=$(grep -m1 '^    mean' "build/frame/turn$Q.txt")
  printf '%-6s %-9s %10s %10s %10s %10s\n' "$Q" "$DPS" \
    "$(echo "$L"|awk '{print $2}')" "$(echo "$L"|awk '{print $4}')" \
    "$(echo "$L"|awk '{print $6}')" "$(echo "$L"|awk '{print $10}')"
done
