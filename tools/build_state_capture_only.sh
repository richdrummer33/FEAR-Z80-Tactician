#!/usr/bin/env bash
set -euo pipefail
GEAR="${1:-}"
if [[ -z "$GEAR" || ! -f "$GEAR/src/GearsystemCore.cpp" ]]; then
  echo "usage: $0 /path/to/Gearsystem-3.9.16" >&2
  exit 2
fi
ROOT="$(cd "$GEAR" && pwd)"
PORT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$PORT/build/coredbg"
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
for src in "${SRCS[@]}"; do ARGS+=("$ROOT/src/$src"); done
g++ -std=c++17 -O2 -Wall -Wextra "${INC[@]}" \
  "$PORT/tools/tilesector_state_capture.cpp" "${ARGS[@]}" \
  "$PORT/build/coredbg/emu2413.o" "$PORT/build/coredbg/miniz.o" -lm \
  -o "$PORT/build/tilesector_state_capture"
echo "STATE_CAPTURE_BUILD_PASS"
