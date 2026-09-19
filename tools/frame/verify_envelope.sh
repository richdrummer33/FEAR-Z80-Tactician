#!/usr/bin/env bash
# Equivalence under an explicit motion envelope.
#
# The committed baseline hashes were captured at the shipping turn rate. A
# retained renderer is exactly the kind of change whose failures hide at the
# rates the baseline never visited, so this rebuilds BOTH the reference and the
# current tree at each rate and compares them against each other.
set -euo pipefail
FRAMES="${1:-150}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
mkdir -p build/frame/env
build_and_hash() {  # $1=trace $2=turnq4 $3=out
  python3 tools/frame/gen_traces.py "$1" build/frame_trace.c >/dev/null
  rm -f build/main_tilesector_polar_gg.o build/tilesector_polar_motion_gg.o \
        build/frame_trace_gg.o build/gg-tilesector-polar.*
  make GBDK_HOME="$GBDK_HOME" POLAR_PROFILE_HOOKS=1 POLAR_LOCAL_PROJECTION=0 \
    POLAR_EXTRA_OBJS=build/frame_trace_gg.o \
    TILESECTOR_FASTFLAGS="-Wf--opt-code-speed -DTSPF_DEFAULT_APPEARANCE=0u -DTSPF_TRACE_INPUT=1 -DMANUAL_TURN_Q4=$2" \
    gg-tilesector-polar >/dev/null
  ./build/mat_census build/gg-tilesector-polar.gg build/gg-tilesector-polar.noi \
    "$1" "$FRAMES" 8 "$3" >/dev/null 2>&1
}
FAIL=0
for Q in ${QLIST:-48 144 320}; do
  DPS=$(python3 -c "print(f'{(($Q+8)>>4)*360/256*20:.0f}')")
  for T in ${TLIST:-cruise spin corners stress}; do
    git stash -q
    build_and_hash "$T" "$Q" "build/frame/env/$T-$Q-ref.hash"
    git stash pop -q
    build_and_hash "$T" "$Q" "build/frame/env/$T-$Q-new.hash"
    if diff -q "build/frame/env/$T-$Q-ref.hash" "build/frame/env/$T-$Q-new.hash" >/dev/null; then
      echo "  ${DPS}deg/s $T: IDENTICAL over $FRAMES frames"
    else
      echo "  ${DPS}deg/s $T: DIFFERS on $(diff "build/frame/env/$T-$Q-ref.hash" "build/frame/env/$T-$Q-new.hash" | grep -c '^<') frames"
      FAIL=1
    fi
  done
done
[ "$FAIL" = 0 ] && echo "ENVELOPE EQUIVALENCE OK" || { echo "ENVELOPE EQUIVALENCE FAILED"; exit 1; }
