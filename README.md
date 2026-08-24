# FEAR-Z80-Tactician

**Current version: v0.5.0 — Stage 5: banked individual GOAP brains**

Stage 5 keeps the original 46 x 24 `office-loop` battlefield and four-vs-five-to-seven
population from Stage 4, then adds the first real FEAR-style autonomous planning layer:
individual goal arbitration plus persistent backward-regressive GOAP. Squad HTN is
intentionally still absent so the CPU and memory cost of the individual brains can be
measured cleanly.

## What is running in the Game Gear ROM

- original 46 x 24 office-loop topology
- four BLUE agents versus seeded five-to-seven RED agents (eleven maximum)
- 4 x 4 pixel logical cells with horizontal camera scrolling
- offscreen agents continue to simulate normally
- imperfect contact memory, LOS, doors, cover, suppression, ammo, reloads and combat
- room-graph long-range navigation plus bounded local BFS fallback
- per-agent goal arbitration with current-goal tie hysteresis
- persistent GOAP plans with concrete target / position bindings stored beside symbolic facts
- backward regression planner; actions are retained until completion or explicit invalidation
- reload -> shoot multi-action plans
- map bit-packing and compact shared navigation scratch
- 64 KiB Sega-mapper ROM using one switchable bank for the brain code

Current individual goals are the subset that does not require squad-task bindings:

- `ESCAPE_DANGER`
- `COVER`
- `KILL_ENEMY`
- `SEARCH_LOST`
- `PATROL`
- `IDLE`

Current GOAP primitives:

- `RELOAD`
- `MOVE_TO_TARGET`
- `SHOOT`
- `MOVE_TO_COVER`
- `MOVE_TO_LAST_SEEN`
- `MOVE_PATROL`
- `WAIT`

The task/HTN-coupled goals such as `MoveToTask`, `ClearRoom`, and full squad manoeuvres
remain future stages by design.

## ROM banking

The build now links a normal 64 KiB Game Gear ROM with GBDK `-autobank` and the Sega mapper.
The high-level individual brain lives in switchable ROM bank 1; simulation primitives,
rendering and the bank-call trampoline stay fixed in bank 0.

Generated symbols prove the bank assignment:

- `___bank_brain_bank = 1`
- `b_brain_run_actor_round = 1`
- `_brain_run_actor_round = 0x14DEE`

One banked call runs the entire actor round. This avoids swapping the mapper separately for
every one of the eleven agents.

`romusage` for the final seed-2 ROM:

- fixed ROM bank 0: 12,684 used / 3,700 free
- switchable bank 1: 3,766 used / 12,618 free
- total GG WRAM identified by the CDB: 1,908 used / 6,284 free
- ROM image: 65,536 bytes / four 16 KiB banks

Banks 2 and 3 are presently essentially expansion room. The project does not depend on an
SD filesystem at runtime; an SD flashcart merely supplies the normal `.gg` ROM image.

## Data compaction

The authored map is no longer stored as a byte per logical cell in RAM.

- 46 x 24 = 1,104 map cells
- walls: one ROM bit per cell = 138 bytes
- dynamic door open/closed state: one byte total for seven doors
- door coordinates remain a tiny separate authored table
- a second 138-byte ROM door-presence bitset is intentionally kept as a CPU accelerator so
  ordinary floor/wall queries do not scan all seven door coordinates

Shared local-BFS scratch was also reduced from roughly 3.3 KiB in the earlier representation
to about 926 bytes:

- visited: 138-byte bitset
- parent direction: 276 bytes at two bits per cell
- bounded X/Y queue: 256 + 256 bytes

This is why adding persistent GOAP state to eleven agents still leaves most of WRAM free.

## Host verification

`make test` validates the authored map, actor invariants and nine deterministic seeds. It also
contains a focused planner test that forces an agent to face a visible enemy with an empty
magazine. Backward regression produces:

`RELOAD -> SHOOT`

The first decision reloads and advances the persistent plan. The second decision reuses that
same plan and shoots rather than replanning.

Final seed-2 host reference:

- population: BLUE 4 / RED 7
- winner: RED
- completion tick: 59
- final RED survivors / HP: 5 / 13
- total replans: 273
- total persistent-plan reuses: 269

## Actual Game Gear / Gearsystem verification

The final 64 KiB ROM is identified by Gearsystem as:

- Game Gear
- valid cartridge header
- Sega mapper
- four 16 KiB ROM banks
- NTSC / 60 Hz

Seed 2 (maximum eleven actors) reaches the same final state as the host reference.
Gearsystem first exposes `done=1` at video frame 2237:

- tick 59
- RED wins
- total replans 273
- total plan reuses 269

A complete early actor round was directly observed with `last_acted_mask = 0x07FF`, meaning
all eleven actor bits were set.

Persistent-plan evidence from actual GG RAM for BLUE agent 0:

- frame 380 / tick 11: `SEARCH_LOST`, destination (20,16), action `MOVE_TO_LAST_SEEN`,
  replan count 7, reuse count 4, position (9,11)
- frame 420 / tick 12: same goal, destination and action, replan count still 7,
  reuse count 5, position advanced to (10,11)

So the plan is not merely recomputed into the same answer: the per-agent persistent plan is
being retained and executed across logical ticks on the Z80.

At frame 1024 / tick 27 the same agent is in `KILL_ENEMY` with a two-step
`RELOAD -> SHOOT` plan; the reload has completed, ammo is 3 and `plan_pos = 1`.

The final performance pass moved goal-context work out of the per-goal scoring loop, avoids
repeated visibility queries, adds the door-presence accelerator, and performs one banked
brain call per world tick instead of up to eleven. The seed-2 completion point fell from
about frame 3156 in the first banked prototype to frame 2237 in this final build: about a
29 percent reduction in video frames for the same deterministic match.

Average over the full match is about 37.9 video frames per logical tick, or about 1.58
complete eleven-agent world ticks per second at 60 Hz. Individual ticks vary because local
pathfinding is deliberately an on-demand fallback.

A second seed-42 ROM also matches its host reference: ten actors, RED winner at logical tick
69; Gearsystem first reports done at frame 2154.

## Outputs

- `roms/FEAR-Z80-Tactician-v0.5.0-seed2.gg` — committed primary seed-2 / maximum 4v7 ROM
- `roms/FEAR-Z80-Tactician-v0.5.0-seed42.gg` — committed seed-42 4v6 ROM
- `roms/SHA256SUMS-v0.5.0.txt` — checksums for the committed ROM snapshots
- generated `.cdb`, `.noi`, `.map`, framebuffer captures and FFmpeg recordings remain reproducible local build artifacts

See `docs/releases/v0.5.0-runtime-verify.txt` for the compact verification record and `docs/VERSIONING.md` for the artifact naming policy.

## Build

```sh
make test
make gg GBDK_HOME=/path/to/gbdk
make gg-seed42 GBDK_HOME=/path/to/gbdk
make release GBDK_HOME=/path/to/gbdk
```

The GG link uses:

```text
-mz80:gg -debug -autobank -Wb-ext=.rel -Wl-j -Wm-yo4
```

## Controls

- START: pause / unpause
- RIGHT while paused: single logical tick
- A: restart with the next seed
- B: cycle tick divider 1 / 2 / 4

## Scope line

This stage proves **banked autonomous individual GOAP brains**, not the complete desktop FEAR
AI yet. Squad HTN, task bindings, breach semantics, grenades, richer suppression actions,
weapon profiles, morale and medical behavior are deliberately still outside this stage.


## Versioning

The repository uses `vMAJOR.MINOR.PATCH`. During the pre-1.0 port, capability stages map to the minor version, so Stage 5 is `v0.5.0`. Curated ROMs are intentionally committed with version + seed in the filename. See `docs/VERSIONING.md` and `docs/PROJECT_MEMORY.md`.
