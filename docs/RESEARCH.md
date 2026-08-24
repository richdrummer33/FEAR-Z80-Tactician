# Game Gear implementation research

Research checked 2026-08-23. Primary sources are preferred where available.

## Hardware constraints that shape the design

### CPU and RAM

- Game Gear CPU is a Z80-family CPU clocked at about **3.579545 MHz**.
- System RAM is **8 KiB**, mapped at `0xC000-0xDFFF` and mirrored at `0xE000-0xFFFF`.
- Cartridge ROM occupies the lower address space and larger ROMs use banking.

Sources:
- SMS Power! Game Gear official documentation: https://www.smspower.org/Development/GameGearOfficialDocs
- SMS Power! memory map: https://www.smspower.org/Development/MemoryMap
- GBDK banking docs: https://gbdk.org/docs/api/docs_rombanking_mbcs.html

### Display and VRAM

The Game Gear's visible LCD is **160 x 144 pixels**, conveniently **20 x 18 tiles** at 8 x 8 pixels. The underlying SMS/GG VDP name table is larger; GBDK exposes the visible Game Gear crop as:

- `DEVICE_SCREEN_X_OFFSET = 6`
- `DEVICE_SCREEN_Y_OFFSET = 3`
- `DEVICE_SCREEN_WIDTH = 20`
- `DEVICE_SCREEN_HEIGHT = 18`
- hardware tilemap buffer `32 x 28`

GBDK's high-level background tile setters already account for the Game Gear visible-screen offset. The port therefore uses visible logical coordinates directly; manually adding `(6,3)` as well would apply the crop twice. That double-offset bug was found and removed during the first real GBDK ROM bring-up.

Game Gear has **16 KiB VRAM** and native 4bpp background tiles. GBDK describes two 16-color palettes; Game Gear palette entries are BGR444 with 4 bits per R/G/B component.

Sources:
- GBDK supported consoles: https://gbdk.org/docs/api/docs_supported_consoles.html
- GBDK SMS/GG hardware source: https://github.com/gbdk-2020/gbdk-2020/blob/develop/gbdk-lib/include/sms/hardware.h
- SMS Power! VRAM map: https://www.smspower.org/Development/VRAMMap

## Chosen compiler/toolchain: GBDK-2020

GBDK-2020 officially targets Sega Game Gear. For Game Gear, the documented lcc target is:

```text
-mz80:gg
```

GBDK's `lcc` wrapper drives patched SDCC, assembler, linker and ROM finalization. The project intentionally uses `-debug`, which asks the compiler/linker for debug and symbol outputs.

Stable release observed: **GBDK 4.5.0**. Its documentation explicitly says the z80 targets require GBDK-patched SDCC; the GBDK repository warns Linux users not to substitute the distro's ordinary SDCC package.

Sources:
- Getting started: https://gbdk.org/docs/api/docs_getting_started.html
- Cross compiling: https://gbdk.org/docs/api/docs_supported_consoles.html
- Toolchain settings: https://gbdk.org/docs/api/docs_toolchain_settings.html
- Toolchain overview: https://gbdk.org/docs/api/docs_toolchain.html
- 4.5.0 release notes: https://gbdk.org/docs/api/docs_releases.html
- 4.5 migration notes: https://gbdk.org/docs/api/docs_migrating_versions.html
- patched SDCC releases: https://github.com/gbdk-2020/gbdk-2020-sdcc/releases


### ROM banking now used by Stage 5

Stage 5 links a 64 KiB Game Gear image with GBDK `-autobank`. The Sega memory map keeps a fixed 16 KiB code region and exposes switchable 16 KiB cartridge windows; banked code is placed in the switchable code window. The high-level individual GOAP brain is assigned to bank 1 and entered once per actor round, while rendering and simulation primitives remain fixed. Gearsystem identifies the resulting image as a four-bank Sega-mapper Game Gear cartridge.

The SD card in a flashcart is storage for ROM images; it does not turn the SD capacity into directly addressable Z80 RAM. Larger programs are still structured as banked cartridge ROM.

Source: local GBDK 4.5.0 banking documentation (`docs/pages/05_banking_mbcs.md`) and `banks_autobank` example.

## Emulator/debugger choice

### Preferred for this project: Gearsystem

Gearsystem is an open-source SMS/Game Gear emulator with a serious debugger: CPU/memory breakpoints, disassembler, symbols, memory editor, trace logger, I/O inspection and VRAM viewers. Recent builds also include an **MCP server** with headless mode. That is unusually attractive for an AI-assisted iteration loop because the emulator can be controlled and inspected without a visible desktop session.

Current release observed during research: **3.9.16**, released 2026-08-22. The Linux desktop builds may require SDL3.

Sources:
- repository/readme: https://github.com/drhelius/Gearsystem
- releases: https://github.com/drhelius/Gearsystem/releases
- MCP docs: https://github.com/drhelius/Gearsystem/blob/master/MCP_README.md

### Excellent fallback: Emulicious

Emulicious runs on Java and explicitly supports Game Gear. Its debugger includes source files, reverse stepping, profiler, tracer, coverage analysis, memory/palette/tile/tilemap viewers and remote debugging. This sandbox already has Java and `xvfb-run`, so an uploaded Emulicious distribution is likely the easiest GUI-debugger fallback here.

Sources:
- https://emulicious.net/
- https://emulicious.net/downloads/
- https://emulicious.net/home/emulicious-debugger/

### RetroArch

Still useful on the user's machine, particularly once the ROM is stable. Gearsystem also has a libretro core. For bring-up, however, standalone Gearsystem or Emulicious is more useful because we want symbols, VRAM inspection, breakpoints and profiling rather than only running the ROM.

## Renderer strategy

A software framebuffer is the wrong fight for this first slice. The VDP already wants tiles, and the simulation's abstraction is a grid. So:

- 1 logical cell = 1 native 4bpp background tile.
- solid tile index doubles as visual category/color.
- agents are background cells for v0; no sprite budget at all.
- only changed cells are sent to the VDP after initialization.
- the top map row is already an impassable border, so it can visually become the HUD without deleting walkable world space.

This also leaves sprites completely free for later transient effects if they prove worthwhile.

## Memory discipline

The port uses:

- `uint8_t` cell storage, not C enum arrays;
- no `malloc` / `new`;
- no STL;
- no recursion;
- shared navigation scratch rather than per-agent path buffers;
- one deterministic PRNG state;
- compact agent state.

The host layout gives `sizeof(Sim) == 404` bytes. Shared BFS scratch is 1,080 bytes. This strongly suggests the 1v1 core is viable inside 8 KiB, but final proof comes from the SDCC/GBDK link map plus `romusage -p:SMS_GG`.

## Current environment status

The two uploaded archives were inspected directly.

### Gearsystem: solved

`Gearsystem-3.9.16.tar.gz` is source, but that is sufficient. The libretro core builds here with the existing GCC/G++ toolchain and does **not** need SDL3. A tiny libretro host now runs ROMs and captures frames. In addition, Gearsystem's portable core sources build into a custom native runner that can inspect Z80 registers and mapped memory directly. This gives us a practical headless debugging loop even without the full SDL/MCP desktop frontend.

A hand-emitted Game Gear smoke ROM has already passed that loop: valid GG Export header, 160x144 frames, expected VDP crop, and a RAM canary observed through Gearsystem after execution.

### GBDK: one file still missing

The uploaded `gbdk-2020-4.5.0.tar.gz` is GitHub's **source-code archive**. GBDK's own build instructions say the source tree expects a separately supplied, patched SDCC build via `SDCCDIR`; ordinary distro SDCC is explicitly not suitable. The archive contains GBDK sources and docs but no `sdcc`, `sdasz80`, `sdldz80`, or ready-to-run `lcc` toolchain.

So either of these will finish the compiler side:

1. **Preferred:** GBDK 4.5.0 release asset `gbdk-linux64.tar.gz` (the prebuilt Linux x64 toolchain).
2. **Also sufficient:** the matching GBDK-patched SDCC Linux x64 bundle for 4.5.0; with that, the already uploaded GBDK source tree can be built locally by setting `SDCCDIR`.

Nothing else is needed from the original simulator for the first ROM. Once the binary toolchain arrives, the next command is the real `lcc -mz80:gg -debug` link, followed immediately by Gearsystem execution, frame comparison, Z80/RAM inspection and ROM/RAM usage auditing.
