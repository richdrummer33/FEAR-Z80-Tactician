#!/usr/bin/env bash
# Build and run the Z80 raster-kernel race.
#
#   GBDK_HOME=... tools/race/build_and_run.sh [iterations]
#
# Needs .toolchain/gbdk and .toolchain/Gearsystem, the same two the live rung
# workflow installs. The profiler refuses to report timings unless every kernel
# produces byte-identical output, so a fast wrong kernel cannot win.
set -euo pipefail
ITER="${1:-4}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
ROOT="${GEARSYSTEM_ROOT:-$PWD/.toolchain/Gearsystem}"
mkdir -p build/race

python3 tools/race/gen_race_data.py tools/race/race_data.h

# The selector tables depend on the race cases, so they are regenerated here and
# the same run prints the ROM accounting for the two record layouts.
cc -O2 -Itools/race -Itools/rung1 -Isrc -o build/gen_selector_tables tools/race/gen_selector_tables.c -lm
./build/gen_selector_tables tools/race | tee build/race/selector-tables.txt

SEL="tools/race/sel_b0.c tools/race/sel_b2.c tools/race/sel_b3.c tools/race/sel_b4.c tools/race/sel_b5.c tools/race/sel_b6.c"

# host correctness first: a build-and-emulate cycle is a bad place to find a bug
cc -O2 -Itools/race tools/race/race_host_check.c $SEL -o build/race_host_check
./build/race_host_check

# -Wm-yo8 because the selector tables live in fixed Frame-2 banks 2..6, which is
# how the real thing would have to hold them: the whole domain is ~73 KB.
"$GBDK_HOME/bin/lcc" -mz80:gg -debug -Wb-ext=.rel -Wl-j -Wm-yo8 -Itools/race \
  -Wf--opt-code-speed -o build/race/race.gg \
  tools/race/race_kernels.c tools/race/race_asm.s $SEL

if [ ! -x build/race_profile ] || [ tools/race/race_profile.cpp -nt build/race_profile ]; then
  g++ -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz" \
    tools/race/race_profile.cpp \
    $(ls "$ROOT"/src/*.cpp "$ROOT"/src/audio/*.cpp | grep -v "main\|Gearsystem.cpp") \
    build/coredbg/emu2413.o build/coredbg/miniz.o -lm -o build/race_profile
fi
./build/race_profile build/race/race.gg build/race/race.noi "$ITER"
