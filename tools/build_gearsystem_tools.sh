#!/usr/bin/env bash
set -euo pipefail
GEAR="${1:-${GEARSYSTEM_DIR:-}}"
if [[ -z "$GEAR" || ! -f "$GEAR/src/GearsystemCore.cpp" ]]; then
  echo "usage: $0 /path/to/Gearsystem-3.9.16" >&2
  exit 2
fi
ROOT="$(cd "$GEAR" && pwd)"
PORT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PORT/build/coredbg"

echo "[1/5] Building Gearsystem libretro core (no SDL required)..."
make -C "$ROOT/platforms/libretro" -j"$(nproc)"

echo "[2/5] Building tiny libretro frame runner..."
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I"$ROOT/platforms/libretro" \
  "$PORT/tools/libretro_runner.cpp" -ldl -o "$PORT/build/libretro_runner"

echo "[3/5] Building native Gearsystem core debugger runner..."
INC=(-I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz")
gcc -std=gnu99 -O2 "${INC[@]}" -c "$ROOT/src/audio/emu2413/emu2413.c" -o "$PORT/build/coredbg/emu2413.o"
gcc -std=gnu99 -O2 "${INC[@]}" -c "$ROOT/platforms/shared/dependencies/miniz/miniz.c" -o "$PORT/build/coredbg/miniz.o"
SRCS=(
  Audio.cpp Cartridge.cpp CodemastersMemoryRule.cpp GameGearIOPorts.cpp GearsystemCore.cpp Input.cpp
  KoreanMemoryRule.cpp KoreanMSXSMS8000MemoryRule.cpp KoreanSMS32KB2000MemoryRule.cpp KoreanMSX32KB2000MemoryRule.cpp
  Korean2000XOR1FMemoryRule.cpp KoreanMSX8KB0300MemoryRule.cpp Korean0000XORFFMemoryRule.cpp KoreanFFFFHiComMemoryRule.cpp
  KoreanFFFEMemoryRule.cpp KoreanBFFCMemoryRule.cpp KoreanFFF3FFFCMemoryRule.cpp KoreanMDFFF5MemoryRule.cpp KoreanMDFFF0MemoryRule.cpp
  Multi4PAKAllActionMemoryRule.cpp IratahackMemoryRule.cpp JumboDahjeeMemoryRule.cpp Eeprom93C46MemoryRule.cpp
  Memory.cpp MemoryRule.cpp MSXMemoryRule.cpp opcodes.cpp opcodes_cb.cpp opcodes_ed.cpp Processor.cpp RomOnlyMemoryRule.cpp
  SegaMemoryRule.cpp SG1000MemoryRule.cpp SmsIOPorts.cpp Video.cpp BootromMemoryRule.cpp JanggunMemoryRule.cpp YM2413.cpp
  VgmRecorder.cpp TraceLogger.cpp audio/Blip_Buffer.cpp audio/Sms_Apu.cpp audio/Stereo_Buffer.cpp
)
ARGS=()
for s in "${SRCS[@]}"; do ARGS+=("$ROOT/src/$s"); done
COMMON=("${ARGS[@]}" "$PORT/build/coredbg/emu2413.o" "$PORT/build/coredbg/miniz.o" -lm)
g++ -std=c++17 -O2 -Wall -Wextra "${INC[@]}" \
  "$PORT/tools/gearsystem_core_runner_native.cpp" "${COMMON[@]}" \
  -o "$PORT/build/gearsystem_core_runner"

echo "[4/5] Building TileSector instruction profiler..."
g++ -std=c++17 -O2 -Wall -Wextra "${INC[@]}" \
  "$PORT/tools/tilesector_profile_runner.cpp" "${COMMON[@]}" \
  -o "$PORT/build/tilesector_profile_runner"

echo "[5/5] Building TileSector first-write tracer..."
g++ -std=c++17 -O2 -Wall -Wextra "${INC[@]}" \
  "$PORT/tools/tilesector_write_trace.cpp" "${COMMON[@]}" \
  -o "$PORT/build/tilesector_write_trace"

echo "Built:"
echo "  $ROOT/platforms/libretro/gearsystem_libretro.so"
echo "  $PORT/build/libretro_runner"
echo "  $PORT/build/gearsystem_core_runner"
echo "  $PORT/build/tilesector_profile_runner"
echo "  $PORT/build/tilesector_write_trace"
