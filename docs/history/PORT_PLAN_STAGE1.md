# Port plan and desktop-to-Game-Gear mapping

## What the original desktop project is doing

The attached desktop project is a C++17 ASCII CQB simulator with F.E.A.R.-shaped layers: perception/memory, goal arbitration, persistent backward-regressive GOAP, primitive actions, plus shallow squad/mission HTN behavior.

The useful desktop anchors inspected for this port are in `src/main.cpp`:

- `Sim` begins around line 1840.
- `importantLog()` around line 1934 requests event-driven pauses.
- map builders begin around line 2075.
- `tickOnce()` around line 5582 advances world systems, shuffles living units and lets eligible units decide/execute.
- CLI help/parser around lines 6714-6750 includes `--auto-play`.
- interactive loop around line 6781 honors event pauses unless `--auto-play` is enabled.

The first Game Gear slice deliberately copies **semantics**, not implementation weight.

## Desktop -> GG slice

| Desktop concept | GG v0 equivalent |
| --- | --- |
| Perception | Bresenham LOS |
| memory | last seen x/y + tick |
| individual goal arbitration | 4 tiny goals: PATROL/HUNT/ATTACK/COVER |
| persistent GOAP | omitted for v0 |
| squad HTN | omitted for 1v1 |
| primitive actions | move/open door/shoot |
| terminal map | 20x18 background tilemap |
| verbose logs | compact HUD + colored event flashes |
| event auto-pause | omitted; run continuously |
| cooldown-driven eligibility | removed; every living agent acts every tick |
| shuffled initiative | deterministic alternating first mover |

## Phase 0 — completed here

- Pure C fixed-memory core.
- 1v1.
- exact visible-screen map.
- host behavior preview.
- deterministic invariant tests.
- Game Gear renderer/input adapter.

## Phase 1 — real ROM + debugger

Once GBDK is available in the environment:

1. Link `build/cqb_1v1.gg` with `lcc -mz80:gg -debug`.
2. Inspect `.map`, `.noi` / `.cdb`, and `romusage -p:SMS_GG`.
3. Run the ROM in Gearsystem headless/MCP if its Linux bundle is available; otherwise Emulicious under Xvfb.
4. Verify visible crop, tile palette, controls and frame pacing.
5. Step hundreds of frames and compare winner/tick against the host core for the same seed when input is untouched.

## Phase 2 — visual language

Keep the zero-sprite baseline unless sprites prove genuinely useful.

- Current: floor/wall/door/team colors; hit, miss/tracer and door flashes; compact stats.
- Next: alternating HUD/event ticker on the top border without reducing traversable map area.
- Optional: one- or two-frame directional streak tile for gunfire and door-opening pulse.
- Optional: encode remembered enemy location as a dim team-colored blink when out of sight.

## Phase 3 — a little more F.E.A.R. without summoning the RAM demon

Good candidates, in order:

1. directional facing + cheap vision cone;
2. two or three cover postures instead of scanning every cover tile every decision;
3. one grenade type with a fixed-radius lookup and no event pause;
4. tiny action plans of 2-4 steps, represented as enum bytes rather than a graph search;
5. 2v2 after profiling.

Avoid importing the desktop GOAP data structures directly. If a planner returns later, use a tiny bounded action-state search with a hard node cap and compact bit-packed facts.

## Phase 4 — larger-than-screen / Kill-Team-ish dimensions

The Game Gear VDP has a larger hardware name-table buffer than the visible 20x18 crop. For maps larger than the screen, keep the simulation grid independent and scroll a viewport over it. Do not tie world dimensions to the visible tilemap once v0 is stable.

The first-screen arena should remain as a regression test forever: if a later planner or renderer cannot run Crossfire Cloister cheaply, the port has become too fat.
