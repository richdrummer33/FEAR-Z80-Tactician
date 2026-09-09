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

### A7. Decode-clip kernel — CLOSED. Cycle-exact, 6,000/6,000 verified.

`tools/z80_decode_bench.py` (`make span-decode-bench`) is a real Z80
implementation of `project_key`'s visibility clip
(`src/tilesector_polar_renderer.c:416-427`): assembled from a from-scratch
mini-assembler, run on a from-scratch cycle-exact interpreter, verified
bit-for-bit (visibility decision AND exact lo/hi window value, not just
aggregate stats) against **6,000 real (a0,a1,yaw) triples** sampled from
real block spans at real poses.

Built in two layers on purpose. The Z80 has no native 16-bit signed compare,
so every comparison in this kernel reduces to one primitive - "is HL < DE
(signed)?", answered by XORing bit 15 of both operands (which maps two's
complement ordering onto plain unsigned ordering) then reading the Carry flag
from an ordinary `SBC HL,DE`. That primitive was self-tested against 4,052
cases (every boundary value the kernel actually compares against, plus 4,000
random int16 pairs) **before** it went anywhere near the real kernel. Only
after that passed at 0 failures was the full clip logic built on top of it.

Two real bugs were caught by the verification loop, not by inspection:

1. **Prefix-matching order** in the mini-assembler: `"jp "` matched before
   the more specific `"jp z,"`, silently mis-parsing every conditional jump.
2. **`XOR A`** (the register-form idiom for zeroing A, used in the
   reject/visible branches) was being parsed as `XOR <label "a">` by the
   immediate-operand path, since the assembler didn't special-case it.

And one real logic bug survived assembly and was only caught by the
oracle-comparison loop: the final `hi<=lo` reject test used `HL=hi, DE=lo`
with "jump to visible if not-carry" - which admits `hi==lo` as visible. The
C reference requires **strict** `hi>lo`. Fixed by testing `lo<hi` directly
and jumping to visible only on Carry. 23 of the first 6,000 test cases
caught this before the fix; 0 after.

**Measured: 919.7 T/span** (min 143, max 1,048) - not the 262-317 T
instruction-counted estimate this replaces. **190% over the top of that
estimate**, the same failure mode as the original emit estimate (94% low)
but more severe. The wrap-loop-to-plain-`if` simplification used here is
provably exact for all inputs (not just this map's geometry) - see the
module docstring for the range-bound proof that `en`'s and `st`'s adjustment
loops can fire at most once each, which is why they were implemented as
straight-line conditionals rather than actual loops.

### A8. GATE selector kernel — CLOSED. Cycle-exact, 6,000/6,000 verified.

`tools/z80_gate_bench.py` (`make span-gate-bench`) is `selector_pass()`
(`v = sel_a*lx + sel_b*ly + sel_c; pass = (v>=0) ^ sel_inv`) as real Z80,
built with the same two-layer discipline: an isolated multiply primitive
self-tested first, then the real kernel verified bit-for-bit against
**6,000 real (selector, lx, ly) triples**.

Real coefficient ranges were read from `src/generated`, not assumed:
`sel_a` in [0,29], `sel_b` in [-69,37], `sel_c` in [-3712,1344]. `sel_a`
happens to be non-negative in this map's data but its C type is `int8_t`,
so the multiply primitive (signed-8 x unsigned-6bit -> signed-16, via
sign-extend-then-6-iteration-shift-add) was self-tested across the full
signed 8-bit domain - 4,050 cases, 0 failures - rather than the narrower
range this map happens to exercise.

Three real bugs, all caught by verification:

1. **Sign-extension had source and fill bytes swapped** - the multiplicand
   byte was loaded into the HIGH byte of the 16-bit pair with the sign-fill
   in the LOW byte, backwards from correct two's-complement extension. The
   isolated primitive self-test caught this immediately (3,964/4,050
   failures) before it could reach the kernel.
2. **Two register-form instructions** (`XOR A`, `XOR D`) were being
   misparsed by the assembler's immediate-operand path as `XOR <label>`,
   same class of bug as the clip kernel's `XOR A` issue in A7 - fixed by
   special-casing both register forms.
3. **A test-harness bug, not a kernel bug**: `run_kernel()` read results
   from the *input* memory buffer after execution, not `cpu.m` - the
   `Z80` class copies its input (`self.m = bytearray(mem)`), so the two
   diverge the moment execution starts. Every result read "0" until this
   was fixed. Cross-checked against `z80_decode_bench.py`, which reads
   `cpu.m` correctly and was unaffected - this bug was isolated to the new
   file, not a defect in the already-shipped decode-clip kernel.

**Measured: 863.1 T/gate** (min 787, max 926). No prior instruction-counted
estimate existed specifically for this stage in this document, but the very
first ISA sketch (superseded) guessed ~115 T - **7.5x low**, the same
directional miss as every hand-count in this project so far.

Updated whole-update picture, all three components now cycle-exact:

| stage | T/update | confidence |
| --- | ---: | --- |
| decode-clip (12.00 spans-tested x 919.7 T) | 11,036 | **cycle-exact**, 6,000 cases verified |
| GATE (2.71 gates-tested x 863.1 T) | 2,339 | **cycle-exact**, 6,000 cases verified |
| emit | 21,756 | **cycle-exact**, verified against 30 real viewports |
| **decode-clip + GATE + emit** | **35,131** | excludes bearing lookup and column-solve |

### A9. Bearing-field evaluation kernel — CLOSED. Cycle-exact, 53,112/53,112 verified.

`tools/z80_bearing_bench.py` (`make span-bearing-bench`) evaluates a baked
local affine leaf on real Z80:
`(base + shr0(sx*lx0, shift) + shr0(sy*ly0, shift)) & 4095`, matching
`quant_leaf_record` / `quant_leaf_error` in
`experiments/adaptive_polar_field/local_projection_field_poc.py`. Verified
against **every corner record in the shipped map** (4,426 usable records
x 12 sub-cell probe positions = 53,112 cases), bit-for-bit, at each record's
real chosen leaf depth.

`shr0` is a power-of-two divide rounded **toward zero**, which an arithmetic
shift is not for negative values. Implemented as "normalise sign, shift LEFT
by 8-shift, take H, negate back": `add hl,hl` is 11 T where `srl h; rr l` in
a djnz loop is 29 T *per bit*, so a >>6 costs 48 T instead of ~180 T. The
range is safe **by construction, not by luck** - `|p| <= 127*(2^shift - 1)`,
so `|p << (8-shift)| < 32512` for every depth 0..3. That bound is in the
module docstring, not left implicit.

**Leaf-depth distribution across the real map** (threshold 4 Q12, min leaf 8):

| depth | span | shift | records | share | T/lookup |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 64 | 6 | 3,810 | 85.9% | 1,416.9 |
| 1 | 32 | 5 | 462 | 10.4% | 1,973.6 |
| 2 | 16 | 4 | 123 | 2.8% | 2,085.1 |
| 3 | 8 | 3 | 31 | 0.7% | 2,188.8 |
| — | — | — | 7 | 0.2% | **no depth<=3 meets threshold — see C4** |

**Map-weighted mean: 1,499.0 T per bearing lookup.** 86% of corners need a
single affine plane for the whole 4-world-unit cell — the field is far
smoother than the quadtree machinery assumes.

**The finding that mattered more than the kernel:** naive evaluation costs
**18.17 lookups/update**, but only **9.12 distinct corners** are referenced
(worst case 12) — a measured **1.99x redundancy**, because every corner is a
shared span endpoint. `lx,ly` are fixed for the whole update, so a corner's
bearing *cannot* change within one; a per-update cache of at most 12 entries
(corner id -> Q12 + validity byte) turns every second reference into a ~30 T
table read.

- naive: 27,239 T/update — the largest line in the whole budget
- cached: **13,674 T/update**

This is not a micro-optimisation to file away. It is a required part of the
design and is costed as such below.

### A10. Column-solve workload — MEASURED (op counts), kernel not yet built

`tools/column_solve_workload.py` (`make column-solve-workload`), 14,912 real
poses. Reports 4.30 visible spans/update, **exactly** matching
`span_decode_workload.py` — an independent path arriving at the same number.

14 of this map's 17 segments are cardinal (82.4% statically). At runtime the
shortcut fires more often than that:

| path | share of visible spans | cost |
| --- | ---: | --- |
| `wall_d_q4` cardinal | 87.5% | one Q4 subtraction, zero multiplies |
| `wall_d_q4` general | 12.5% | 4 multiplies |
| `inv_for_dq4` near-clamp | 16.5% | no multiply |
| `inv_for_dq4` far-clamp | 2.9% | no multiply |
| `inv_for_dq4` interpolate | 80.6% | 1 multiply |
| `inv_at_invd` cardinal | 87.5% | dot is a raw trig byte |
| `inv_at_invd` general | 12.5% | 2 extra multiplies |

**Multiplies per update: 7.78 (8x8) + 21.52 (16x8) = 29.30.** The 16x8 count
is the whole story: 5 per visible span (two per `inv_at_invd` endpoint, x2
endpoints, plus the Q6 step), and none of them is deletable by a cardinal
shortcut.

Projected **17,729 T/update** from measured op counts x measured op costs
(432 T for an 8x8, 454 T for a 16x8, derived from the bearing kernel's
cycle-exact 324 T / 6 iterations / 54 T per iteration, x1.35 for branch and
staging overhead). **This is a projection, not a measurement** — and every
projection in this project so far has come in low. Treat 17,729 as a floor.

Deliberately not costed: the `screen_depth_plane` fast path. Its
`k_tspf_depth_normal_class` / `g_depth_nf_q7` / `g_depth_stepfac_q4` tables
are SDCC-side and not in the generated pack, so the *exact endpoint path* was
costed instead. The plane path can only be cheaper; costing the exact path
keeps the budget honest.

### A11. Quarter-square multiply table — the single largest remaining lever

Now measurable rather than speculative. Multiplies dominate every stage:

| stage | multiply loops | measured/projected T |
| --- | ---: | ---: |
| bearing lookup (cached) | 18.24 (2 per distinct corner) | 13,674 |
| column-solve | 29.30 | 17,729 |

A quarter-square table (`a*b = f(a+b) - f(a-b)`, `f(x)=x^2/4`, 512 bytes of
ROM) replaces a ~430 T shift-add loop with two table reads and a subtract —
roughly 60-80 T. At 47.5 multiply loops per update that is **on the order of
17,000 T/update recovered**, which would take the whole update from ~66,500 T
to roughly 35,000 T, i.e. from 0.90 updates/frame to about 1.7.

- Worth: **≈17,000 T/update**, larger than A1, A2 and A4 combined
- Costs: 512 bytes of ROM out of 128 KiB linked
- Closes with: build the table kernel, self-test it across the real operand
  ranges, then re-run `span-bearing-bench` and rebuild column-solve on it
- **Do it before writing the column-solve kernel**, not after — the kernel's
  whole shape depends on whether a multiply costs 430 T or 70 T

### Whole-update budget, current best measurement

| stage | T/update | confidence |
| --- | ---: | --- |
| bearing lookup (cached, 9.12 distinct x 1,499 T) | 13,674 | **cycle-exact**, 53,112 cases verified |
| decode-clip (12.00 spans-tested x 919.7 T) | 11,036 | **cycle-exact**, 6,000 cases verified |
| GATE (2.71 gates-tested x 863.1 T) | 2,339 | **cycle-exact**, 6,000 cases verified |
| column-solve | 17,729 | **projected** from measured op counts — a floor |
| emit | 21,756 | **cycle-exact**, verified against 30 real viewports |
| **TOTAL** | **66,534** | |

One NTSC frame at 59.9 Hz is 59,736 T; VBlank alone is 15,960 T. So the span
interpreter currently costs **0.90 updates per frame** — about **53.8 Hz** of
update rate if the Z80 did nothing else at all, which it must. That is the
honest headline: the architecture works and lands in the right order of
magnitude, and it is *not* comfortably inside budget until A11 lands.

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

### C4. Seven corners have no accurate baked leaf

Surfaced by `make span-bearing-bench`, which counts what the field bake
quietly skips: **7 of 4,433 corner records (0.2%)** have no quadtree depth
<= 3 that meets the 4 Q12 accuracy threshold. `corner_quant_depth` returns
`None` for them and `serialize_quantized_cell` writes `0xff` as the depth
byte — an escape marker with, at present, **nothing on the runtime side that
handles it**.

These are the near-singular cases: a corner essentially on top of the camera,
where bearing changes arbitrarily fast with sub-cell position and no affine
patch can track it. Two honest options, neither chosen yet:

1. An exact fallback path (real `atan2`-equivalent via the existing
   `k_tspf_atan_q12` table) for the 0.2%, gated on the `0xff` marker.
2. Prove those corners can never be visible from inside their own cell (a
   corner that close is behind the near plane or occluded), and make the
   marker a hard assert instead of a fallback.

Option 2 is likely correct and much cheaper, but it is a *claim*, not a
measurement, and this file does not carry claims. Until one of these is
resolved the bearing field is 99.8% baked, not baked.

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
