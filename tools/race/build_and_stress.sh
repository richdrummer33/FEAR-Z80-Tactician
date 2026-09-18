#!/usr/bin/env bash
# Build and run the full-domain selector stress sweep.
#
#   GBDK_HOME=... tools/race/build_and_stress.sh [max_frames]
#
# Two sweeps, and the host one runs first because it is four seconds and the
# emulator one is minutes:
#
#   host      every step, every phase, twelve span lengths, against the
#             closed-form column rule in wide integers. Also translation
#             invariance and the thirteen exceptional multiples of 256.
#   on-device the hand DDA against the production selector on the real
#             cartridge tables: five record banks, a real Frame-2 mapper write,
#             and the hand-written Z80 scan.
set -euo pipefail
FRAMES="${1:-400000}"
: "${GBDK_HOME:=$PWD/.toolchain/gbdk}"
export GBDK_HOME
ROOT="${GEARSYSTEM_ROOT:-$PWD/.toolchain/Gearsystem}"
mkdir -p build/stress

cc -O2 -Itools/race -Itools/rung1 -Isrc -o build/gen_selector_tables tools/race/gen_selector_tables.c -lm
./build/gen_selector_tables tools/race --full > build/stress/tables.txt
tail -20 build/stress/tables.txt

FULL="tools/race/selfull_b0.c tools/race/selfull_b1.c tools/race/selfull_b2.c \
      tools/race/selfull_b3.c tools/race/selfull_b4.c tools/race/selfull_map.c \
      tools/race/selfull_hi.c tools/race/selfull_steps.c"

echo "--- host: exhaustive over the whole step domain ---"
cc -O2 -Itools/race tools/race/selector_exhaustive.c $FULL tools/race/sel_b0.c \
   -o build/selector_exhaustive
./build/selector_exhaustive
echo "--- host: mutation control, each defect must be caught ---"
./build/selector_exhaustive --mutations

echo "--- on-device: the real cartridge path ---"
"$GBDK_HOME/bin/lcc" -mz80:gg -debug -Wb-ext=.rel -Wl-j -Wm-yo16 -Itools/race \
  -Wf--opt-code-speed -o build/stress/stress.gg \
  tools/race/stress_kernels.c tools/race/stress_asm.s $FULL tools/race/sel_b0.c

if [ ! -x build/stress_run ] || [ tools/race/stress_run.cpp -nt build/stress_run ]; then
  g++ -std=c++17 -O2 -I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz" \
    tools/race/stress_run.cpp \
    $(ls "$ROOT"/src/*.cpp "$ROOT"/src/audio/*.cpp | grep -v "main\|Gearsystem.cpp") \
    build/coredbg/emu2413.o build/coredbg/miniz.o -lm -o build/stress_run
fi
./build/stress_run build/stress/stress.gg build/stress/stress.noi "$FRAMES"
