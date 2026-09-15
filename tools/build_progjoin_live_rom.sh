#!/usr/bin/env bash
# Build one experimental 1 MiB playable Game Gear ROM variant for the PROGJOIN
# live A/B rung.
#
#   usage: build_progjoin_live_rom.sh <variant> <outdir> <gendir>
#
# Variants differ ONLY in the compiled finite-program path, so the pair is
# like-for-like: same parity materializer, same live splice, same ROM size,
# bank count, appearance mode and PROGJOIN asset objects are linked in every
# case. That is what makes a name-table A/B between them meaningful.
#
#   baseline       TSPF_PROGJOIN_FULL=0 + STATS=1   legacy edge solver only
#   progjoin       TSPF_PROGJOIN_FULL=1             compiled edges, no counters
#   progjoin-stats TSPF_PROGJOIN_FULL=1 + STATS=1   compiled edges + accounting
#
# The baseline carries the counters as well. They cannot be incremented there
# (the bridge that touches them is compiled out), so reading them back as zero
# measures the absence of compiled playback instead of inferring it from the
# preprocessor. "progjoin" keeps them off so the cycle comparison is against an
# uninstrumented build; "progjoin-stats" must hash identically to it.
set -euo pipefail

VARIANT="${1:?variant}"
OUT="${2:?outdir}"
G="${3:?gendir}"

case "$VARIANT" in
  baseline)       PJ_FLAGS="-DTSPF_PROGJOIN_FULL=0 -DTSPF_PROGJOIN_STATS=1" ;;
  progjoin)       PJ_FLAGS="-DTSPF_PROGJOIN_FULL=1" ;;
  progjoin-stats) PJ_FLAGS="-DTSPF_PROGJOIN_FULL=1 -DTSPF_PROGJOIN_STATS=1" ;;
  *) echo "unknown variant: $VARIANT" >&2; exit 2 ;;
esac

: "${GBDK_HOME:?GBDK_HOME must be set}"
LCC="$GBDK_HOME/bin/lcc"
FLAGS="-mz80:gg -debug -autobank -Wb-ext=.rel -Wl-j -Wm-yo64 -Isrc -I$G -Ibuild/generated/polar_projection -Ibuild/generated/polar_depthplane"
CFLAGS="-Wf--opt-code-speed -DTSPF_PROFILE_HOOKS=1 -DTSPF_LOCAL_PROJECTION=1 -DTSPF_SCREEN_DEPTH_PLANE=1 -DTSPF_DEFAULT_APPEARANCE=0 $PJ_FLAGS -DTSP_PROGJOIN_BANK_BASE=48"

RENDERER_OBJS=(
  build/main_tilesector_polar_gg.o
  build/tilesector_polar_motion_gg.o
  build/tilesector_polar_renderer_gg.o
  build/tilesector_polar_ntstate_gg.o
  build/tilesector_polar_materialize_gg.o
  build/tilesector_polar_ntupload_profiled_gg.o
  build/tilesector_polar_projection_gg.o
  build/tilesector_polar_depthplane_gg.o
)
for i in 0 1 2 3 4 5; do RENDERER_OBJS+=("build/tilesector_polar_proj_bank$i.o"); done

# TSPF_PROGJOIN_FULL reaches the renderer translation unit, so every variant
# needs its own compile of the shared objects, not just its own link.
rm -f "${RENDERER_OBJS[@]}"
make GBDK_HOME="$GBDK_HOME" POLAR_ROM_BANKS=64 \
  POLAR_GGFLAGS="$FLAGS" POLAR_CFLAGS="$CFLAGS" "${RENDERER_OBJS[@]}" >/dev/null

OBJDIR="$OUT/obj-$VARIANT"
mkdir -p "$OBJDIR"
"$LCC" $FLAGS $CFLAGS -c -o "$OBJDIR/progjoin_runtime.o" src/tilesector_polar_progjoin_runtime.c
for f in pj_meta pj_desc pj_records0 pj_records1 pj_body0 pj_body1 pj_body2 pj_body3 pj_body4; do
  "$LCC" $FLAGS $CFLAGS -c -o "$OBJDIR/$f.o" "$G/$f.c"
done

OBJS=("${RENDERER_OBJS[@]}" "$OBJDIR/progjoin_runtime.o")
for f in pj_meta pj_desc pj_records0 pj_records1 pj_body0 pj_body1 pj_body2 pj_body3 pj_body4; do
  OBJS+=("$OBJDIR/$f.o")
done

"$LCC" $FLAGS -Wm-yS -o "$OUT/$VARIANT.gg" "${OBJS[@]}"
test "$(stat -c%s "$OUT/$VARIANT.gg")" -eq 1048576
# makebin emits both ".noi" (NoICE "DEF <sym> <value>") and ".sym"
# ("<bank>:<addr> <sym>"); the profilers read the ".sym" form.
test -s "$OUT/$VARIANT.sym"
sha256sum "$OUT/$VARIANT.gg"
