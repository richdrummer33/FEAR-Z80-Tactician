# Performance ground truth

**Purpose.** One authoritative page so that two identical-looking cycle numbers
from completely different universes stop being compared. Every figure here
carries a noun and a provenance label. If a number is not in this file with a
label, do not quote it.

Last rebuilt: 2026-09-13, at commit `0e8d88c` on branch
`claude/sms-architecture-diagram-law8ti`.

---

## 1. Clock and budget reference

Sega Game Gear Z80 at **3,579,545 Hz**. NTSC display is 59.9 Hz; one display
frame is **59,736 T**, of which VBlank alone is 15,960 T.

| target update rate | cycles available per update |
| ---: | ---: |
| 60 Hz | 59,659 T |
| 40 Hz | 89,489 T |
| 30 Hz | 119,318 T |
| 20 Hz | 178,977 T |

Project gates, from `docs/PROJECT_MEMORY.md`: **179K T/update for 20 Hz is the
solid target; 119K T/update for 30 Hz is aspirational.**

---

## 2. Provenance labels — never blend these

| label | meaning |
| --- | --- |
| **MEASURED TARGET ROM** | A real `.gg` ROM executed in an emulator (Gearsystem/libretro). Includes everything the real machine does. |
| **MEASURED Z80 KERNEL** | One stage, assembled and run on the project's cycle-accurate Z80 interpreter (`tools/z80core.py`), verified against a C oracle. Real cycle counts, but ONE STAGE ONLY, with no VBlank, no VRAM transfer, no game logic. |
| **HOST ORACLE** | C code run on the development machine. Establishes correctness and operation counts. **Carries no cycle meaning whatsoever.** |
| **MODELLED / COMPOSED** | Separately measured pieces added together. No integrated execution. Has been wrong by 37% at least once (see §6). |
| **ESTIMATE** | A projection with no measurement behind it. |

---

## 3. What has actually run on a Game Gear

**The 3D renderer has never run on a Game Gear.** Not once, in any form.

The only ROMs ever built, committed and emulator-verified are the **GOAP
tactical-AI demo** — a 2D top-down squad-simulation, a completely separate
program from the 3D span renderer:

| artifact | what it is | provenance |
| --- | --- | --- |
| `roms/FEAR-Z80-Tactician-v0.5.0-seed2.gg` (65,536 B) | GOAP AI demo, 11 agents | MEASURED TARGET ROM |
| `docs/releases/v0.5.0-runtime-verify.txt` | Gearsystem 3.9.16 run of the above | MEASURED TARGET ROM |
| `docs/history/STAGE4_OFFICELOOP_RUNTIME_VERIFY.txt` | earlier GOAP office-loop run | MEASURED TARGET ROM |

**The "60 fps" in both documents is the ffmpeg capture rate of the emulator's
display output**, i.e. the Game Gear's own NTSC cadence. It is not a renderer
frame rate and never was.

**The "37.9" figure is `2237 / 59 = 37.9` VIDEO FRAMES PER LOGICAL AI TICK** —
the reciprocal of a rate, for the AI, not the renderer. The same document
states the actual rate plainly: **about 1.58 eleven-agent world ticks per
second.**

`docs/TODO_DEFERRED.md` section B carries a standing warning against exactly
this confusion: *"Gearsystem's ~59.9 Hz is the Game Gear display/VBlank
cadence, not the renderer's FPS."* Section B is the harness that would produce
a true renderer update rate from a WRAM counter. It is **specified and
deliberately not started**, gated on there being a renderer running on a Z80
at all.

**Conclusion: no renderer FPS has ever been demonstrated on target. There is no
30 FPS result, no 40 FPS result, and no 37 FPS renderer result.**

---

## 4. The performance lineage

Every row below is the 3D span renderer unless stated. "T/update" means cycles
for one camera pose to be turned into a finished 20x18 name table in RAM.

| # | architecture / benchmark | what it physically does | provenance | T/update | implied rate | confidence |
| ---: | --- | --- | --- | ---: | ---: | --- |
| 1 | GOAP AI demo v0.5.0 | 2D squad AI, no 3D renderer | MEASURED TARGET ROM | n/a | 1.58 world ticks/s | high |
| 2 | Early span-interpreter budget | bearing + decode-clip + GATE + column-solve + "emit" | MODELLED from 4 measured kernels + 1 projection | 48,600 | ~73 updates/s | **void — see note** |
| 3 | First complete pipeline | the same, with the real column materializer built | MODELLED from measured kernels | 275,745 | 12.98 /s | medium |
| 4 | After the materializer optimisation ladder | carry endpoint, carry row pointer, inline compare, register-resident fill | MODELLED, materializer MEASURED Z80 KERNEL | 245,220 | 14.60 /s | medium |
| 5 | After the row-extent walk rewrite | rows by walking, not dividing | MODELLED | 242,103 | 14.79 /s | medium |
| 6 | After the edge-tile table | replace tile-selection arithmetic with a hoisted table read | MODELLED, materializer 175,924 MEASURED | **223,266** | 16.03 /s | medium |
| 7 | Plus the zero-row early-out reorder | test "does this edge draw anything" before computing its slope | MODELLED, materializer 173,257 MEASURED | 220,599 | 16.23 /s | medium |
| 8 | Plus the border hoist | compute the wall-border tile pointer once per run, not per column | MODELLED, materializer 169,548 MEASURED | 216,890 | 16.50 /s | medium |
| 9 | **Corrected: real depth-plane column-solve** | replaces row 8's wrong column-solve line | MODELLED, ALL SIX PARTS MEASURED Z80 KERNEL | **211,308** | **16.94 /s** | **highest available** |
| 10 | Compiled edge-program architecture | baked per-column tile programs replacing the edge path | MODELLED — **the programs have never been generated or linked** | ~103,836 | ~34.5 /s | low |

**Note on row 2, the ~73 updates/s figure.** That budget's fifth line was
"emit", a 21,756 T kernel that rebuilt a name table *from finished words*. The
work of PRODUCING those words did not exist yet. When the column materializer
was built it measured **228,403 T** and, as the record states, *"REPLACES the
emit line, it does not add to it"*. **Row 2 was missing the single most
expensive stage in the renderer, by a factor of ten.** Any memory of the
renderer being near frame rate traces to this row. It is void.

---

## 5. Reconstructing the 223,266 T figure

This is the number quoted throughout the recent work. It is a sum of six
separately measured Z80 kernels:

| component | T/update | provenance |
| --- | ---: | --- |
| bearing lookup | 5,833 | MEASURED Z80 KERNEL |
| decode-clip | 11,036 | MEASURED Z80 KERNEL |
| GATE selector | 2,339 | MEASURED Z80 KERNEL |
| **column-solve** | **26,820** | **MEASURED Z80 KERNEL — but of the WRONG PATH** |
| depth sort | 1,314 | MEASURED Z80 KERNEL |
| materializer (edge-table version) | 175,924 | MEASURED Z80 KERNEL |
| **TOTAL** | **223,266** | MODELLED / COMPOSED |

### Why the column-solve line was wrong

"Column-solve" turns a wall segment plus a camera pose into the two numbers the
materializer walks: a starting inverse-depth and a per-column step.

There are **two implementations of that stage in the shipped C**, and the code
picks between them at runtime:

- **The depth-plane solve.** Two multiplies against a per-yaw coefficient pair,
  then a short walk along the columns. Tried FIRST.
- **The endpoint solve.** Projects both wall endpoints independently through a
  trigonometric chain. Used ONLY when the depth-plane solve reports that the
  wall plane crosses zero inside the visible run.

The 26,820 T kernel implements **the endpoint solve — the fallback.**

Measured over 29,824 camera poses and 127,958 visible wall runs, on the Game
Gear code path: **the depth-plane solve succeeds on 99.10% of runs.** The
fallback runs on **0.90%**.

So the budget line priced a path the target takes fewer than one time in a
hundred.

### The corrected line

A twin kernel was then built — same bearing, clipping and column-range code,
only the solve stage swapped — and verified exact on 6,039 of 6,039 oracle
rows: **4,939.0 T per wall run, 21,238 T/update.**

| | T/update |
| --- | ---: |
| 223,266 as published | |
| − wrong column-solve (endpoint/fallback) | −26,820 |
| + right column-solve (depth-plane/normal) | +21,238 |
| − materializer improvements since (early-out reorder + border hoist) | −6,376 |
| **= current best composed whole update** | **211,308** |

**That is a −5,582 T correction, about 2.5%. It does not change any
conclusion.** The renderer was never close to frame rate in these budgets.

---

## 6. Current best honest budget

### Measured pieces — each a MEASURED Z80 KERNEL, cycle-accurate, oracle-verified

| stage | T/update |
| --- | ---: |
| bearing lookup | 5,833 |
| decode-clip | 11,036 |
| GATE selector | 2,339 |
| column-solve, depth-plane (normal) path | 21,238 |
| depth sort | 1,314 |
| column materializer, border-hoist version | 169,548 |
| **sum** | **211,308** |

### Not in that number at all

- **VRAM transfer.** The materializer writes a name table in RAM. Moving it to
  video memory is a separate cost and appears in **no** budget on this branch.
- Game logic, input handling, VBlank service, audio.
- Any integration overhead between stages.

So 211,308 T is a **floor for the render pipeline alone**, not a frame cost.

### Implied rate

**IMPLIED FROM COMPOSED COST, NOT MEASURED FPS:**
3,579,545 / 211,308 = **16.9 renderer updates per second**, before VRAM
transfer and before any game logic.

Against the project gates: **20 Hz needs 178,977 T. We are 18% over the solid
target and 77% over the 30 Hz aspiration.**

### The unbuilt architecture

The compiled edge-program design would, if built, take the materializer from
169,548 to roughly 62,000 T and the whole update to roughly **103,836 T**,
implying about **34 updates/s**. Its dispatch tables are about 2.9 MB, the
cartridge is currently configured for 64 KiB, and **none of it exists in
`src/`**. This is the lowest-confidence row in this document.

---

## 7. Standing rules

1. The **normal** column-solve path is the depth-plane solve (99.10%). The
   **fallback** is the endpoint solve (0.90%). Never price the fallback as if
   it were the stage.
2. A composed budget is not a measurement. Composing measured component costs
   across a path change mispredicted the depth-plane kernel by **37%**.
3. Display cadence is not renderer frame rate. Video frames per logical tick is
   not frame rate either.
4. No renderer FPS may be claimed until section B's WRAM update counter is
   built and read from a real ROM.
