#!/usr/bin/env bash
# Replay the renderer's real raster tuples through both kernels on real timing.
set -euo pipefail
N="${1:-8000}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
ROOT="${GEARSYSTEM_ROOT:-$PWD/.toolchain/Gearsystem}"
mkdir -p build/corpus build/race

if [ ! -f build/race/corpus_tuples.csv ]; then
  cc -O2 -Itools/race -Itools/rung1 -Isrc src/tilesector_polar_motion.c tools/race/span_census.c \
     -o build/span_census -lm
  ./build/span_census 2 build/race/corpus_tuples.csv | tail -14
fi
python3 tools/race/gen_corpus.py build/race/corpus_tuples.csv tools/race "$N"

FULL="tools/race/selfull_b0.c tools/race/selfull_b1.c tools/race/selfull_b2.c \
      tools/race/selfull_b3.c tools/race/selfull_b4.c tools/race/selfull_map.c \
      tools/race/selfull_hi.c tools/race/selfull_steps.c"
CORP="tools/race/corpus_b9.c tools/race/corpus_b10.c tools/race/corpus_b11.c"

"$GBDK_HOME/bin/lcc" -mz80:gg -debug -Wb-ext=.rel -Wl-j -Wm-yo16 -Itools/race \
  -Wf--opt-code-speed -o build/corpus/corpus.gg \
  tools/race/corpus_kernels.c tools/race/stress_asm.s $FULL tools/race/sel_b0.c $CORP

if [ ! -x build/corpus_run ] || [ tools/race/corpus_run.cpp -nt build/corpus_run ]; then
  g++ -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz" \
    tools/race/corpus_run.cpp \
    $(ls "$ROOT"/src/*.cpp "$ROOT"/src/audio/*.cpp | grep -v "main\|Gearsystem.cpp") \
    build/coredbg/emu2413.o build/coredbg/miniz.o -lm -o build/corpus_run
fi
./build/corpus_run build/corpus/corpus.gg build/corpus/corpus.noi "$N"
