#!/usr/bin/env bash
# Build and run the Z80 raster-kernel race.
#
#   GBDK_HOME=... tools/race/build_and_run.sh [iterations]
#
# Needs .toolchain/gbdk and .toolchain/Gearsystem, the same two the live rung
# workflow installs. The profiler refuses to report timings unless all three
# kernels produce byte-identical output, so a fast wrong kernel cannot win.
set -euo pipefail
ITER="${1:-4}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
ROOT="${GEARSYSTEM_ROOT:-$PWD/.toolchain/Gearsystem}"
mkdir -p build/race

python3 tools/race/gen_race_data.py tools/race/race_data.h

# host correctness first: a build-and-emulate cycle is a bad place to find a bug
cc -O2 -Itools/race tools/race/race_host_check.c -o build/race_host_check
./build/race_host_check

"$GBDK_HOME/bin/lcc" -mz80:gg -debug -Wb-ext=.rel -Wl-j -Wm-yo4 -Itools/race \
  -Wf--opt-code-speed -o build/race/race.gg tools/race/race_kernels.c tools/race/race_asm.s

if [ ! -x build/race_profile ]; then
  g++ -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz" \
    tools/race/race_profile.cpp \
    $(ls "$ROOT"/src/*.cpp "$ROOT"/src/audio/*.cpp | grep -v "main\|Gearsystem.cpp") \
    build/coredbg/emu2413.o build/coredbg/miniz.o -lm -o build/race_profile
fi
./build/race_profile build/race/race.gg build/race/race.noi "$ITER"
