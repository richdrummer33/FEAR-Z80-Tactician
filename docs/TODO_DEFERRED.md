# Deferred work — stowed deliberately, not forgotten

Items here are **understood, measured where possible, and consciously postponed**.
They are not open questions. Each entry states what it is, what it is worth, and
what would close it.

Two rules for this file, learned the hard way:

1. If it is only in a CI log or a chat reply, it does not exist. Numbers live here.
2. Nothing is removed until it is either done or explicitly abandoned with a reason.

---

## A. Z80 span-interpreter micro-architecture

Stowed after the emit kernel was measured cycle-exactly
(`make span-emit-bench`, `tools/z80_emit_bench.py`). The big architectural
questions are answered; these are the remaining cycle-level wins.

**Baseline:** emit measured at **21,756 T/update**, 60.43 T per name-table word,
over 360 words emitted unconditionally, verified word-for-word against 30 real
host-oracle viewports.

**Decode-stage baseline** (`tools/span_decode_workload.py`, `make
span-decode-workload`, 14,912 real poses): mean 4.30 spans visible per update,
7.70 tested-but-rejected (**64.1% of tested spans are clip-rejected**), 2.71
GATE evaluations. Cross-validated against the independent `polar-test`
regression's avg_runs=4.06 (different pose sampling entirely) - agreement
within 6% is evidence the block-bake model is faithful to the real renderer,
not proof of an exact match.

A bug was caught and fixed while building this: the first port of
`project_key`'s wrap-around loop used +/-2048 as the threshold instead of the
real code's +/-512 (`src/tilesector_polar_renderer.c:426`). Re-measuring after
the fix changed the reported statistics by under 0.3% (4.29->4.30 visible,
64.3%->64.1% rejected) - on this map's short wall segments the wrap loop
essentially never needs more than one iteration, so both thresholds converge
to the same answer in practice. Still a real correctness bug, worth fixing
before a map with longer angular spans makes it matter. Caught by re-reading
source against the port rather than trusting the first pass - the discipline
this file exists to enforce.

### A1. LITERAL opcode — largest remaining win

A single-word run costs **86 T** through the run path (pop, count test, pop,
store, djnz, loop) against **41 T** for a direct pop-and-store.

Measured run-length distribution: mean 3.56, **median 1**. 1,624 of 3,030
sampled runs are a single word, because every tile row the wall silhouette
crosses needs its own sub-offset edge tile.

- Worth: **≈2,400 T/update** (54.1 singletons × ≈45 T)
- Closes with: a LITERAL opcode plus dispatch, then re-run `span-emit-bench`
- Expected result: ≈19,400 T

### A2. ×4 partial unroll of the fill loop — smaller than it looks

- Worth: **1,587 T/update** (10.3% of fill), projected analytically from the
  measured run distribution
- Why it disappoints: runs are too short to complete quads. This is the
  measured refutation of the intuition that loop overhead dominates.
- Do it *after* A1, since LITERAL removes exactly the runs unrolling cannot help

### A3. Retained emit vs unconditional emit — an open A/B, not a decision

The ISA spec argued for deleting the dirty/coverage machinery because a
720-byte screen fits in 72% of one NTSC VBlank. **At the measured 60.43 T/word
that argument is weaker than when it was made.**

| | cost |
| --- | ---: |
| emit all 360 words | 21,756 T |
| emit only changed words (mean 80.3 per adjacent pose, from the transition bake) | ≈4,800 T + cost of knowing which |

- Closes with: an A/B of both emit paths against the same pose sequence
- **Do not treat the "delete the dirty machinery" recommendation as settled.**

### A4. 2-byte run records — measured, not worth it

Putting the word's high byte in a register and shrinking records from 4 bytes
to 2 saves only **≈307 T/update**, because 70% of runs change the high byte
anyway and would need an explicit set. Recorded so nobody re-derives it.

### A5. Kernel in RAM for self-modifying immediates

Cartridge ROM cannot self-modify. Copying the ~400-byte kernel into WRAM
(6.7 KiB free) lets K, shade, stride and tile bases become immediates:
`ld de,NNNN` at 10 T against 16 T+ for a memory load. Also enables
page-aligning the LUTs so a lookup is `ld l,a` with no 16-bit add (~11 T per
lookup, ~4 lookups per column).

### A7. Decode-stage T-state cost — instruction-counted only, NOT cycle-exact

`tools/span_decode_workload.py` now prints a decode-stage T-state estimate
using the same hand-count methodology the ORIGINAL emit estimate used - and
that methodology was measured **94% low** once actually cycle-exact-simulated
(`make span-emit-bench`: 11,200 T estimated vs 21,756 T measured). This
decode estimate has not received that treatment and should be trusted
proportionally less.

Current instruction-counted range: **3,457-4,117 T/update**, applied to the
measured 12.00 spans-tested and 2.71 gates-tested per update. Two things
would tighten it before it is worth trusting:

1. **A cycle-exact decode kernel**, built the same way `z80_emit_bench.py`
   was: a real Z80 instruction stream for the clip arithmetic
   (`project_key`'s len/wrap/window test), assembled and simulated, verified
   against `visible_window()`'s output on real poses. This is the correct
   next step and was deliberately not attempted in the same pass as the
   estimate above - writing untested 16-bit signed-comparison Z80 idioms
   (which the part has no native instruction for) and trusting a hand-count
   of them repeats exactly the mistake the emit estimate made.
2. **The real runtime SPAN-vs-SPANC mix.** `span_decode_workload.py` counts
   spans tested and visible, not which of those were `SPAN` (fresh bearing
   lookup, ~317 T) vs `SPANC` (reused, ~262 T) *at the poses actually
   sampled* - the 44.5% SPANC ratio used above is the bake's overall
   corpus-wide ratio (`span_block_bake.py`), not a per-pose measurement.

Combined with the cycle-exact emit measurement, confidence-labeled:

| stage | T/update | confidence |
| --- | ---: | --- |
| decode | 3,457-4,117 | instruction-counted estimate |
| emit | 21,756 | **cycle-exact**, verified against 30 real viewports |
| column-solve | not yet costed | wall_d_q4, inv_for_dq4, inv_at_invd, Q6 start/step |
| **decode+emit only** | **25,213-25,873** | excludes column-solve entirely |

### A6. K under cylindrical projection

Under rectilinear projection the wall top is exactly a straight line, so one K
per span is exact — but K depends on yaw and must be computed per span
(≈60 T with a quarter-square table). Under a cylindrical mapping yaw becomes a
shift and K becomes bakeable, but wall tops become cosine arcs needing a
second-order walker (`y += d; d += dd`, two adds). Both are cheap; neither is
measured.

---

## B. Runtime capture, telemetry and overlay harness

Specified in full but **not implementable in the current sandbox**:

- `ffmpeg` is **not installed** — no encoding path
- the Gearsystem source tarball returns **403 from the egress proxy** — no
  emulator to build
- GBDK 4.5.0 *is* reachable (HTTP 200), so ROM builds are possible in CI

The existing `polar-explore-video.yml` workflow already installs both in CI, so
this belongs there rather than in a local run.

The intent is that future polar bake tests supply **only a ROM and optionally an
input script**; capture, timing, telemetry, screenshots and overlay stay generic
and unchanged between renderer experiments.

### B1. Deterministic input injection in the capture runner

`tools/gearsystem_vfr_capture.cpp` currently returns zero from its
`input_state_cb`, so every capture is a static camera.

- Accept a scripted joypad timeline (per-VBlank input sequence)
- Feed it through the existing `input_state_cb`
- Record the active input state alongside each timing sample
- Purpose: replay the identical camera/movement test across polar ROM builds

### B2. Fixed WRAM telemetry block in the polar runtime

Ordinary RAM writes only. **Never rendered to the GG screen in benchmark
builds.** Suggested fields:

- magic / version
- VBlank / frame counter
- renderer-completed update counter
- simulation tick, if distinct
- camera X / Y / Z
- yaw / heading
- current cell / state (optional)
- render workload / debug counters (optional)

### B3. Read telemetry once per VBlank in the capture tool

Reuse the mechanism `tools/libretro_ram_probe.cpp` already demonstrates. Emit
`telemetry.csv` synchronised with `timing.csv`.

**This is what gives true renderer-update FPS.** Framebuffer-hash change rate is
a proxy that only works for a moving camera.

### B4. Host-side overlay during encoding

Composited by FFmpeg or a post-process step — **zero Z80/VDP cost**. Overlay:
emulated VBlank number / display Hz, framebuffer-change FPS, true renderer FPS
from WRAM, current frame hold in VBlanks, runtime tick, camera position + yaw,
current scripted input, ROM/build identifier or short hash.

### B5. Configurable capture termination

Keep the existing stop-after-unchanged-framebuffer behaviour for automated
tours; allow it to be disabled for interactive and static test cases.

### B6. One packaged command

    run-polar-runtime-test <rom.gg> <input-script> <duration>

Outputs: clean native capture, telemetry-overlay MP4, lossless MKV,
`timing.csv`, `telemetry.csv`, selected screenshots, ROM SHA-256 + build
metadata.

### Gate — when this section becomes actionable

**Do not write the capture workflow or capture-runner changes until there is
something running on the Z80 (real or interpreted-faithfully) producing a
drawn frame to point the camera at.** Confirmed explicitly by the project
owner. Writing CI/emulator-facing code with no way to execute or verify it
locally is exactly the failure mode this file exists to avoid repeating.
Section B stays fully specified and ready; it is not started.

### Standing distinction — do not conflate these

Gearsystem's ~59.9 Hz is the **Game Gear display/VBlank cadence**, not the
renderer's FPS. Framebuffer-change rate is useful for moving-camera tests. The
**WRAM renderer counter is the authoritative runtime update-rate metric.**

---

## C. Renderer correctness debts carried forward

### C1. Polar vs TileSector oracle divergence

Polar still differs from the mature TileSector oracle: mean ≈51.6% of
name-table words, framebuffer ≈37% at tick 64 and ≈9.7% at tick 128.
First-divergence tracing puts the cause upstream — the earliest sample cell is
never written by Polar while legacy projects geometry into it. That is a
visibility/projection gap, **not** dirty-state corruption, and absence of
geometry is explicitly not dismissed as cosmetic.

### C2. Sweep-order entry point — CLOSED, no rule needed

Originally flagged as an open question: where should the interpreter *start*
the cyclic bearing-ordered walk for a given yaw? Resolved by re-reading
`project_key()` (`src/tilesector_polar_renderer.c:416`) rather than assuming:
the current runtime does not search for a start offset at all. It evaluates
every candidate key unconditionally and rejects per-span via the yaw-relative
window test (`len==0||len>=2048` / `hi<=lo`). No binary search, no wraparound
bookkeeping.

The span interpreter can do the same: walk the full block every frame
(mean 13.25 spans/cell, `span_block_bake.py`) and let each `SPAN`/`SPANC`
self-reject. That per-span clip cost is already inside the "span decode +
setup" line of the emit-bench budget — this was a stitching task, not a new
unknown.

### C3. SPANC safety across yaw-driven culling — CHECKED, safe

Raised and resolved in the same pass: does `SPANC` correctly reuse a corner
bearing from a predecessor that was itself culled off-screen this frame?

Confirmed safe by source inspection. `project_key()` reads both corner
bearings (`a0=g_corner_bearing_q12[v0]; a1=g_corner_bearing_q12[v1];`) from the
baked field *before* the yaw-relative visibility test can reject the span.
Bearing lookup is a precondition of every instruction's visibility test, never
a consequence of passing it — so a culled predecessor still leaves its
right-vertex bearing available for the next `SPANC` to reuse. No runtime
fallback-to-SPAN needed when the predecessor is off-screen.

---

## D. Repository hygiene

### D1. Nothing is merged

1,297 commits across 85 branches, **one merge commit in the entire history**,
and `main` holds 6 commits (the v0.5.0 import plus four docs commits). The
approved compiled-patch architecture is documented on a branch with no code and
implemented on branches never merged. See the git-forensics write-up.

### D2. Stop branching for CI re-runs

`workflow_dispatch` already exists in these workflows. Eight `proof-run`
branches for one showcase is how a readable history becomes unreadable.
