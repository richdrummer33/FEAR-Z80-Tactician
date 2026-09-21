#!/usr/bin/env bash
set -euo pipefail

GEAR="${1:-${GEARSYSTEM_DIR:-}}"
if [[ -z "$GEAR" || ! -f "$GEAR/src/GearsystemCore.cpp" ]]; then
  echo "usage: $0 /path/to/Gearsystem-3.9.16" >&2
  exit 2
fi

ROOT="$(cd "$GEAR" && pwd)"
PORT="$(cd "$(dirname "$0")/.." && pwd)"
CORE_DIR="$PORT/build/coredbg"
OBJ_DIR="$CORE_DIR/obj"
CORE_LIB="$CORE_DIR/libgearsystem_core.a"
mkdir -p "$CORE_DIR" "$OBJ_DIR"

INC=(-I"$ROOT/src" -I"$ROOT/platforms/shared/dependencies/miniz")

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

echo "[1/10] Building Gearsystem libretro core (no SDL required)..."
make -C "$ROOT/platforms/libretro" -j"$(nproc)"

echo "[2/10] Building tiny libretro frame runner..."
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I"$ROOT/platforms/libretro" \
  "$PORT/tools/libretro_runner.cpp" -ldl -o "$PORT/build/libretro_runner"

echo "[3/10] Compiling Gearsystem portable core once..."
rm -rf "$OBJ_DIR"
mkdir -p "$OBJ_DIR"
CORE_OBJS=()

gcc -std=gnu99 -O2 "${INC[@]}" -c \
  "$ROOT/src/audio/emu2413/emu2413.c" -o "$OBJ_DIR/emu2413.o"
CORE_OBJS+=("$OBJ_DIR/emu2413.o")

gcc -std=gnu99 -O2 "${INC[@]}" -c \
  "$ROOT/platforms/shared/dependencies/miniz/miniz.c" -o "$OBJ_DIR/miniz.o"
CORE_OBJS+=("$OBJ_DIR/miniz.o")

for src in "${SRCS[@]}"; do
  obj_name="${src//\//_}"
  obj_name="${obj_name%.cpp}.o"
  obj="$OBJ_DIR/$obj_name"
  g++ -std=c++17 -O2 -Wall -Wextra "${INC[@]}" -c "$ROOT/src/$src" -o "$obj"
  CORE_OBJS+=("$obj")
done

rm -f "$CORE_LIB"
ar rcs "$CORE_LIB" "${CORE_OBJS[@]}"

# Keep these historical paths for older helper scripts/workflows that still
# reference them directly. The archive above is the canonical common core.
cp "$OBJ_DIR/emu2413.o" "$CORE_DIR/emu2413.o"
cp "$OBJ_DIR/miniz.o" "$CORE_DIR/miniz.o"

build_native() {
  local label="$1"
  local source="$2"
  local output="$3"
  echo "$label"
  g++ -std=c++17 -O2 -Wall -Wextra "${INC[@]}" \
    "$PORT/$source" "$CORE_LIB" -lm -o "$PORT/$output"
}

build_native "[4/10] Building native Gearsystem core debugger runner..." \
  tools/gearsystem_core_runner_native.cpp build/gearsystem_core_runner

build_native "[5/10] Building TileSector instruction profiler..." \
  tools/tilesector_profile_runner.cpp build/tilesector_profile_runner

build_native "[6/10] Building TileSector first-write tracer..." \
  tools/tilesector_write_trace.cpp build/tilesector_write_trace

build_native "[7/10] Building external zero-hook polar cadence profiler..." \
  tools/polar_raw_profile_runner.cpp build/polar_raw_profile_runner

build_native "[8/10] Building bottom-up polar function profiler..." \
  tools/polar_function_profile_runner.cpp build/polar_function_profile_runner

build_native "[9/10] Building framebuffer/state capture tool..." \
  tools/tilesector_state_capture.cpp build/tilesector_state_capture

build_native "[10/10] Building exact full-ROM mode cycle profiler..." \
  tools/polar_mode_cycle_profile.cpp build/polar_mode_cycle_profile

echo "[extra] Building update-aligned frame timeline tool..."
build_native "  frame timeline" \
  tools/frame/frame_timeline.cpp build/frame_timeline

echo "Built:"
echo "  $ROOT/platforms/libretro/gearsystem_libretro.so"
echo "  $CORE_LIB"
echo "  $PORT/build/libretro_runner"
echo "  $PORT/build/gearsystem_core_runner"
echo "  $PORT/build/tilesector_profile_runner"
echo "  $PORT/build/tilesector_write_trace"
echo "  $PORT/build/polar_raw_profile_runner"
echo "  $PORT/build/tilesector_state_capture"
echo "  $PORT/build/polar_function_profile_runner"
echo "  $PORT/build/polar_mode_cycle_profile"
echo "  $PORT/build/frame_timeline"
