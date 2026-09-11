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

### A11. Quarter-square multiply table — CLOSED. Built, measured, and much
### smaller than projected — a real correction, recorded rather than quietly
### dropped.

`tools/z80_qsquare_bench.py` (`make span-qsquare-bench`) builds
`a*b = S(a+b) - S(|a-b|)`, `S(n) = floor(n^2/4)`, as a 256-entry x u16
table (512 bytes ROM — the size this entry originally guessed, which was
right) and substitutes it into the bearing kernel's own product step,
re-verified against the **same 53,112-case oracle** `span-bearing-bench`
uses. 53,112/53,112 exact.

Two real bugs, both caught by Layer 1's exhaustive self-test (every p in
[0,128] x every q in [0,63], 8,256 cases — not a sample) before either
reached the substitution step:

1. The 16-bit table-value load `ld a,(hl); ld l,a; inc hl; ld a,(hl); ld
   h,a` clobbers `L` with the loaded byte *before* using `HL` as a pointer
   to fetch the second byte — `inc hl` then increments a corrupted
   pointer. Every case initially failed at ~2x the correct answer. Fixed
   by loading through `DE` (`ld e,(hl); inc hl; ld d,(hl)`, which never
   touches the pointer register) and swapping into place with `ex de,hl`.
2. After that fix, cases only involving zero still failed: the DIFF step's
   `sbc hl,de` reused `DE` left over from the SUM step above it, which by
   then held a stale pointer, not the coordinate. Fixed by reloading the
   coordinate into `DE` immediately before the subtract rather than
   trusting a register to still hold what it held two blocks earlier.

**The projection above was wrong, and this file said so before this
paragraph existed rather than after:** it assumed the 60-80 T textbook cost
of a quarter-square lookup, which presumes page-aligned tables so an index
is a single `ld l,a` (no 16-bit add). This build's table is **not**
page-aligned, so each of the two lookups pays a full `add hl,hl` (double
the sum to a word index) + `add hl,de` (add the table base) + two `(hl)`
reads — about 45-50 T of addressing overhead per lookup, on top of the
16-bit SUM/DIFF arithmetic and sign handling. Measured:

| | T (mean, both products of one lookup) |
| --- | ---: |
| shift-add (`span-bearing-bench`) | 1,499.0 |
| quarter-square table, as built | 1,311.4 |
| **saving** | **187.6 T (12.5%)**, not the ≈26% this entry projected |

Whole-update effect, bearing line only switched (measured):
**66,531 -> 64,820 T**, 0.90 -> 0.92 updates/frame. Nowhere near the
"~35,000 T, 1.7 updates/frame" this entry originally projected — that
number is retracted, not carried forward.

- **A12 (see below) closed the gap this entry blamed on page alignment** —
  and found the larger half of the waste was somewhere else entirely.
- **Do not re-cite this file's original ≈17,000 T claim** as stated. It was
  a projection, and the *first* measurement came in at 187.6 T. A12 then
  recovered 671.8 T more. The lesson is not "the estimate was too
  optimistic" — it is that an estimate of a *lookup* said nothing about the
  code wrapped around the lookup, which is where the time actually was.

### A12. Page-aligned byte planes + byte-sized arithmetic — CLOSED. 57.3% off the bearing lookup.

Same file, `make span-qsquare-bench`, LAYER 3. Verified twice over:
**30,720 exhaustive cases** (every `int8` slope x every valid coord x all
four depths — the complete input domain, not a sample) and then the same
**53,112-case** real-map oracle A11 was held to. Both clean.

Two changes, and the second mattered more than the first:

1. **Page-aligned byte planes.** `S_lo` at page `0x10`, `S_hi` at page
   `0x11`. A lookup becomes `ld l,a` / `ld h,0x10` / `ld e,(hl)` / `inc h` /
   `ld d,(hl)`. No `add hl,hl` to form a word offset, no `add hl,de` to add
   a base. `inc h` crossing from the low plane to the high plane is a
   *load-bearing layout requirement*, not a convenience — asserted at build
   time in the source so a future relocation cannot silently break it.
2. **Every index is one byte, so stop using register pairs.** `|slope|` <=
   128, `coord` <= 63, therefore `sum` <= 191 and `|diff|` <= 128. All of it
   fits in `A`. The A11 build computed abs, sum and diff in 16-bit pairs and
   staged each through memory at 13-16 T per touch, for quantities that
   never needed a second byte. Sign is not stored in a flag byte either —
   the product's sign is just the slope's sign, so bit 7 of `(SLOPE)` is
   re-tested at the end, which costs 21 T and frees `B` for the shift loop.

| primitive | T per bearing lookup (both products) | vs baseline | kernel |
| --- | ---: | ---: | ---: |
| shift-add loop (`span-bearing-bench`) | 1,499.0 | — | — |
| quarter-square, word table + 16-bit math (A11) | 1,311.4 | −12.5% | 142 B |
| **quarter-square, byte planes + byte math (A12)** | **639.6** | **−57.3%** | **71 B** |

Half the time *and* half the code. A12's own contribution over A11 is
**671.8 T (51.2%)** — 3.6x what page-alignment alone was worth in A11's
framing, because most of it was the 16-bit staging, not the addressing.

**Whole-update budget, bearing line switched (measured):**

| primitive | bearing line | TOTAL | updates/frame |
| --- | ---: | ---: | ---: |
| shift-add loop | 13,671 | 66,531 | 0.90 |
| word table (A11) | 11,960 | 64,820 | 0.92 |
| **byte planes (A12)** | **5,833** | **58,693** | **1.02** |

**The span interpreter fits inside one NTSC frame for the first time** —
58,693 T against 59,736 T — with column-solve still carried at its
unmeasured projection. That is a threshold worth naming, and equally worth
not overselling: it assumes the Z80 does nothing else, and one of the five
lines in it has never been built.

The standing generalisation, now that it has bitten in both directions:
**a T-state estimate of an idea is not an estimate of an implementation.**
Every number in this project that was hand-counted has been wrong, low by
94% and 190% (emit, decode-clip) and high by 3.6x (A11's read of its own
opportunity). Only the built-and-verified ones have held.

### A13. Unsigned 8x8 primitive for column-solve — CLOSED. Exhaustive over the entire domain.

Same file, LAYER 4. Column-solve's multiplies are a different shape from the
bearing lookup's: both operands run to 255, so the sums exceed one byte and
A12's table is too small. Ranges read from the C reference and its generated
tables rather than assumed — `invd*dot` is 255x127 (sum <= 382), `q*sec` is
253x~181 (sum <= 434), `(inv1-inv0)*recip` is 255x255 (sum <= 510).

The table therefore grows to 512 entries. `S(511) = 65,280` still fits a u16
with 255 to spare — asserted at build time, not hoped for. Byte planes become
512 bytes each (1 KiB ROM total), `S_lo` across pages P/P+1 and `S_hi` across
P+2/P+3, so the plane hop is `inc h` twice.

**A12's trick survives the ninth index bit**, because that bit is exactly the
carry `add a,c` already produced:

    add a,c / ld l,a / ld a,P / adc a,0 / ld h,a

`ld` does not disturb flags, so the carry is still live two instructions
later. That costs 11 T over the 8-bit-index form — it is not a different
lookup strategy. The DIFF index needs none of it: `|a-b| <= 255` for any two
bytes, so it always lands in the first page and keeps the cheap `ld h,P`.

**Measured 193.0 T, verified on all 65,536 u8 x u8 pairs — the complete
input domain, with no sampling anywhere in it.**

Do not read 193 T (A13) against 334 T (A12) as "unsigned is faster". They do
different work: A13 is a bare product, A12 additionally strips and reapplies
a sign and performs the `shr0` shift. Use A13 where the operands really are
unsigned and no shift follows.

**Column-solve re-costed on it.** Op counts from `column_solve_workload.py`
(29.30 multiplies/update over 14,912 real poses), per-op cost from this
kernel:

| | column-solve |
| --- | ---: |
| old projection (shift-add, 454 T/multiply, derived) | 17,729 T |
| re-costed (A13, 193 T/multiply, measured) | **7,636 T** |

| stage | T/update | confidence |
| --- | ---: | --- |
| bearing lookup (A12) | 5,833 | cycle-exact |
| decode-clip | 11,036 | cycle-exact |
| GATE | 2,339 | cycle-exact, **still on the old primitive** |
| column-solve | ~~7,636~~ | **REFUTED — measured 19,529, see A14** |
| emit | 21,756 | cycle-exact |
| ~~TOTAL~~ | ~~48,600~~ | **superseded: 60,493, 0.99 updates/frame** |

Called a floor at the time, and it was one. The kernel gluing the measured
op counts together did not exist, and the glue is exactly where this
project's surprises have lived — emit's hand-count missed by 94% and
decode-clip's by 190%. A14 built it: **156% over this projection.** The
multiplies were costed correctly; the 1.35x overhead factor was not, and
the real figure is about 4.5x.

### Whole-update budget, current best measurement

| stage | T/update | confidence |
| --- | ---: | --- |
| bearing lookup (cached, A12 byte planes, 9.12 distinct x 639.6 T) | 5,833 | **cycle-exact**, 53,112 + 30,720 cases verified |
| decode-clip (12.00 spans-tested x 919.7 T) | 11,036 | **cycle-exact**, 6,000 cases verified |
| GATE (2.71 gates-tested x 863.1 T) | 2,339 | **cycle-exact**, 6,000 cases verified |
| column-solve (A13 re-cost; was 17,729 on the old primitive) | 7,636 | **projected**, both inputs measured — a floor |
| emit | 21,756 | **cycle-exact**, verified against 30 real viewports |
| **TOTAL** | **48,600** | **1.23 updates/frame** |

One NTSC frame at 59.9 Hz is 59,736 T; VBlank alone is 15,960 T. So the span
interpreter now costs **1.02 updates per frame** — it fits inside a frame for
the first time, at about **61.0 Hz** of update rate if the Z80 did nothing
else at all, which it must.

Read that with the caveats attached, because they are large: **one of the
five lines has never been built.** Column-solve is carried at a projection
(17,729 T) derived from measured op counts on the *old* shift-add primitive.
It has 29.30 multiplies/update against the bearing lookup's 18.24, so it has
more to gain from A12's primitive than the bearing lookup did — but it also
has more room to come in over its projection, which is what every unbuilt
estimate in this project has done. Building that kernel is the next thing
that changes this table.

Two further stages are still on the old shift-add primitive and would each
shrink if rebuilt on A12: **GATE** (2,339 T, two products per gate — the
same shape the bearing kernel had) and the general-path multiplies inside
column-solve. Neither is re-measured here; both are cheap re-runs once the
column-solve kernel exists.

### A14. Column-solve kernel — CLOSED, and the projection was 156% low.

`tools/z80_column_solve_bench.py` (`make column-solve-bench`) is the
`dq4 -> invd -> inv0/inv1` chain as real Z80: `wall_d_q4` (both the cardinal
and general paths), `inv_for_dq4` (all three branches plus the `invz`
interpolation), and `inv_at_invd` twice. 880 bytes.

**The oracle is the shipped C, not a port of it.** `tools/column_solve_probe.c`
`#include`s `tilesector_polar_renderer.c` directly, which makes its `static`
internals callable **without modifying one line of shipped renderer source**,
and dumps what the real code computes on real poses. It also self-checks: the
`lo`/`hi` it re-derives to feed `inv_at_invd` must reproduce the `inv0`/`inv1`
the real `project_key` just returned, or it aborts rather than emitting a
plausible-looking wrong row. That check passed on **all 215,292** visible
spans. The kernel then matched `invd`, `inv0` and `inv1` on **10,765/10,765**
sampled rows.

> The probe walks all 71 keys per pose where the runtime walks only the
> current cell's block, so 215,292 is a **correctness corpus**, deliberately
> broader than the runtime's workload. Do not divide it by the pose count and
> read that as spans/update — that number is 4.30, from
> `span_decode_workload.py`.

**Measured 6,243.0 T/span → 26,845 T/update** for the complete chain.

| column-solve | T/update | error |
| --- | ---: | ---: |
| projection on the shift-add primitive | 17,729 | — |
| re-cost on the A13 primitive (A13) | 7,636 | — |
| measured, invd/inv0/inv1 only | 19,529 | +156% over the A13 re-cost |
| **measured, complete chain** | **26,845** | **+252%** |

**Why the projection failed, precisely.** It costed 29.30 multiplies/update at
193 T and multiplied by 1.35 for "branches, table reads, staging". The
multiplies are real and that unit cost is right — but they account for only
**~18%** of the kernel's measured time. The true overhead factor is about
**4.5x, not 1.35x**. It goes on operand staging through memory (every `MA`/`MB`
is a 13 T store plus a 13 T load), `call`/`ret`, the branch chains selecting
cardinal vs general, and the shifts. Costing an algorithm by counting its
arithmetic and adding a fudge factor does not work on this part, and this is
the third time that has been demonstrated (emit 94% low, decode-clip 190%
low, column-solve 156% low).

One self-inflicted cost was found and fixed before recording the number, since
it was my error rather than the architecture's: the first draft used `djnz`
loops of `srl h; rr l` for the `>>7` and `>>4` steps, at 203 T for a `>>7`,
after the bearing kernel (A9) had already established the fast form. The `>>7`
here **cannot** use A9's shift-left-then-take-H trick — `q*sec+64` reaches
64,579, so a left shift overflows 16 bits and silently corrupts the result.
The exact decomposition `v>>7 == (v>>8)*2 + (low>>7)` holds for any 16-bit `v`
and costs 59 T. That fix alone was worth 748 T/span (5,292.9 -> 4,541.6).

### Whole-update budget — every line now measured

| stage | T/update | confidence |
| --- | ---: | --- |
| bearing lookup (A12) | 5,833 | cycle-exact |
| decode-clip | 11,036 | cycle-exact |
| GATE | 2,339 | cycle-exact, **still on the old primitive** |
| column-solve (complete chain) | 26,845 | **cycle-exact**, vs the shipped C |
| emit | 21,756 | cycle-exact |
| **TOTAL** | **67,809** | **0.88 updates/frame** |

**The sub-frame crossing A12 claimed is withdrawn.** 67,809 T against a
59,736 T frame is 0.88 updates/frame, not 1.23. A12's 1.23 was carrying
column-solve at a projection this entry refuted twice over - first by
building the kernel, then by finishing it. The crossing was an artifact of
unmeasured lines, exactly as that entry warned it might be.

**Efficiency work is explicitly parked here.** The project's priority is
function over T-states: get a whole update running end to end on the Z80
first, then optimise something that demonstrably works. Known-but-unpursued
efficiency leads are logged in A15 rather than chased.

**The chain is now complete.** `angle_x` (both endpoints, plus the swap and
the `x1==x0` widen) and the Q6 `iq`/`step` computation were both in nobody's
budget; they are now inside this kernel and inside its number. The kernel
takes a clipped span and produces everything `draw_run` needs:
**`invd inv0 inv1 x0 x1 c0 c1 n iq step` — all ten verified exact on
10,765/10,765 oracle rows.** 1,189 bytes.

Those two additions cost 1,701 T/span (4,541.6 -> 6,243.0), which is most of
why this line grew. They were never free; they were just never counted.

A third bug, caught the same way: `k_col_recip_q8` is `uint8_t` and the C
promotes it to `int16_t`, so 255 means 255 - but it was fed to the *signed*
multiply, which read it as -1 and flipped the sign of every `step` whose
reciprocal has bit 7 set. 160 of 1,077 rows failed on `step` alone while the
other nine outputs were already exact. Fixed with a separate unsigned-by-
signed primitive (`smulw_u`). Worth noting how it presented: nine of ten
outputs correct is exactly the shape a sign bug takes, and only a
field-by-field oracle comparison surfaces it.

Live leads, in the order they are worth taking:

1. **Register-passing for the multiply operands.** ~65 T of the ~193 T
   multiply is staging `MA`/`MB` through memory, and it happens 5-6 times per
   span. Worth an estimated 300-400 T/span — but that is an estimate, and this
   entry is about what estimates are worth.
2. **Re-run GATE on the A12 primitive** (2,339 T, same two-product shape the
   bearing kernel had).
3. **emit is now the largest single line at 21,756 T** — A1 (LITERAL opcode,
   ≈2,400 T) and A3 (retained vs unconditional emit) are back on the table.

### A24. Materializer optimisation ladder — 22.9% off, every rung twin-verified.

Four changes, each an isolated A/B against the previous rung, each
**2,327/2,327 runs exact** against the per-run oracle. `make
materialize-run-bench` runs the whole ladder.

| rung | T/column | vs previous |
| --- | ---: | ---: |
| NOCARRY twin (reference) | 8,983.5 | — |
| CARRY_EDGE_A — carry the endpoint | 8,762.5 | −2.5% |
| **ROWPTR_B** — carry the name-table pointer | 8,210.5 | **−6.3%** |
| **INLINECMP_C** — inline the signed compare | 7,539.3 | **−8.2%** |
| **FILLLOOP_D** — register-resident interior fill | **6,759.4** | **−10.3%** |
| | | **−22.9% cumulative** |

**ROWPTR_B.** Both hot loops walk rows by +1, so the destination advances by
exactly +40. `row_addr` recomputed `r*40` from scratch every row — 156 T of
shifts plus CALL and RET. Computed once per loop instead, then +39 (the two
stores already advanced +2). It returned less than `row_addr`'s 14.6% profile
share because the loops are short (~2.7 rows), so the one-time hoist only
amortises partly.

*Caught by the twin:* the first attempt read as **+4.9% slower**. The
replacement had missed on a case difference — `0xC027` vs `0xc027` — so the
per-row `CALL` was never removed and the hoist was pure added cost. Without a
twin that would have been recorded as "pointer carry does not help".

**INLINECMP_C.** `cmps` was 16.3%, and almost none of it was the comparison:
CALL (17) + RET (10) + a second push/pop pair (21) wrapping ~70 T of work.
Inlined the identical bias-then-SBC primitive. DE is no longer preserved —
every site reloads it before comparing and none reads it after, but that is an
assertion about the code and the twin is what settles it.

**FILLLOOP_D.** The re-profile promoted `df_loop` to the top at 15.7%, and
almost none of that was the fill: per row it reloaded the pointer from memory,
reloaded both halves of a word that never changes, wrote the pointer back, and
ran a compare-based loop test costing ~70 T alone. Everything it needs fits in
registers — HL pointer, BC word, DE stride, A count — so nothing touches
memory but the two stores that are the actual work.

**The ranking reordered after every single change**, which is the whole
argument for re-profiling rather than working down a list:

| routine | before | after INLINECMP_C | after FILLLOOP_D |
| --- | ---: | ---: | ---: |
| `cmps` | 16.3% | inlined | inlined |
| `row_addr` | 14.6% | 4.7% | 5.2% |
| `df_loop` | 10.7% | **15.7%** | 5.5% |
| `de_loop` | 5.6% | 7.5% | **8.3%** |

**Budget now:** materialize 197,878 T, whole update **245,220 T, 0.24
updates/frame** (from 0.20).

**Next rung.** The profile is now flat — the top item is 8.3% and the top ten
sum to ~54%. There is no single dominant target left, which means per-rung
returns will shrink and the remaining wins are in the edge path as a whole
(`de_loop` + `edge_entry` + `ee_hi_ok` + `shr3_u` + `de_yl_min` ≈ 27%).
That path is the DDA candidate: carrying integer row + sub-row + fractional
error would remove `shr3_u` and `row_floor` outright. It is now the largest
*coherent* target even though no single routine dominates.

**And the flatness is itself the signal** that the standing direction was
right: at 6,759 T/column against emit's 60 T/word, the remaining gap is no
longer obviously implementation slack. Further large wins probably need the
architectural move — stop materializing unchanged cells at all — rather than
more instruction-level work.

### A34. Sparse span stream — exact, 3.7x smaller, and SLOWER as a replacement. Useful as a pre-check.

`make span-stream` and `make union-stream`. Two experiments: is the span the
right retained unit instead of the column, and can the projection path become
ROM lookup?

**The premise about pixel-precision edges is void, and that simplifies things.**
The proposal assumed an edge X is one byte — 5 bits of tile column plus 3 of
sub-tile pixel — and that several edges share a tile. `draw_run` reads only
`x0>>3` and `x1>>3` and nothing else touches `x0`/`x1`. The span footprint is
**tile-column granular**, so an edge is 5 bits and "two edges in one tile" is
just two spans whose `c0` matches. Not asserted from the source — proven by
reconstruction below.

**The record is six bytes and it is complete.**

    sid | inv0 | inv1 | c0 | c1 | flags        (flags = left_real, right_real)

**Exactness gate: rebuild the whole 20x18 name table from those six bytes per
span and nothing else — 0 mismatches in 1,789,440 frames.** Identity needs a
seventh byte, the key id, because **sid is not unique**: 1,340 frames carry two
visible spans with the same sid, and matching on it silently pairs the wrong
spans.

| stream size, U=1 all regimes | |
| --- | ---: |
| visible spans/frame | mean **3.19**, median 2, p95 9, max 13 |
| boundary events/frame | 6.39 |
| events sharing a tile with an earlier one | 50.1% |
| column-materializations/frame | 23.90 |
| retained bytes, span form | **19.2** |
| retained bytes, column form | 71.7 |
| ratio | **3.74x smaller** |

| temporal change per update | |
| --- | ---: |
| spans unchanged | 1.26 (**39.6%**) |
| spans changed | 1.87 (58.6%) |
| appearing / vanishing | 0.06 / 0.07 |
| changed spans | median 1, p95 8, max 13 |
| left edge stationary, of changed spans | 60.0%; when it moves, mean 1.07 columns |
| columns belonging to changed spans | 13.88 of 23.90 |
| **within a changed span, columns whose own state differs** | **82.6%** |

Cross-checks against A30: 39.6% unchanged plus 60% of the changed with a
stationary edge is 74.8% of spans not crossing a column, against A30's 74.4%
measured independently.

**Then the Z80 A/B, and the answer is not the one the host figures suggest.**
`UNION_S` replaces the retained columns with the span stream and derives each
column's heights from the same `iq/step` walk `draw_run` uses. Verified against
a stricter oracle than A33's — the set of cells that ACTUALLY changed between
the two rendered frames, not the host classifier's own mask.

| variant | U=1 rotation | all regimes | marks | changed | exact |
| --- | ---: | ---: | ---: | ---: | :-: |
| UNION_C (A33, column form) | 84,747 | 55,796 | 97.92 | 72.05 | yes |
| UNION_S_A (span stream) | 130,963 | 66,132 | 269.53 | 72.05 | yes |
| UNION_S_B (span stream, edge ranges) | 155,557 | 78,115 | 71.22 | 72.05 | **NO** |

**The span stream is 1.24x to 1.55x SLOWER.** It compares 19.12 bytes instead
of 71.57 — and then has to recompute two clamped 16-bit shifts per column to
recover the heights the column form simply stored. The comparison saving is
about 600 T; the recomputation costs several thousand. **The expensive axis is
per-column derivation, and the span form adds to it in order to save on an axis
that was never expensive.**

Worth recording: UNION_S_B marks 71.22 cells against 72.05 actually changed —
essentially perfect precision — and still misses, because edge-only marking
cannot cover the whole-column cases (border change, column entered or left).
Precision was never the problem.

**What DOES pay is the record as a PRE-CHECK, not as a replacement.** `UNION_E`
is `UNION_C` plus one thing: compare the 6-byte span record before touching any
column, and skip the span entirely if it matches.

| corpus | UNION_C | UNION_E | |
| --- | ---: | ---: | ---: |
| U=1 all regimes | 55,796 | **47,603** | **−14.7%** |
| U=1 pure rotation | 84,747 | 86,107 | +1.6% |

Under rotation almost every span changes, so the pre-check never fires and is
pure overhead. Across realistic motion it removes 10.02 of 23.90 columns from
consideration for six byte-compares. **Keep it, and keep it conditional in
spirit: it is a bet on motion being mixed rather than all-turning.**

**Against the DDA_G baseline the union is still 31.0% (all regimes) to 56.1%
(rotation) of a full render before any drawing.** A33 said roughly 3x was
needed. The span stream delivered 1.15x on the mixed corpus and nothing under
rotation.

### The projection lookup tables — exact, and worth about 1.7% of the update

Domain enumerated over 59,648 poses and 512,028 endpoint evaluations rather
than assumed.

**`inv_for_dq4` is a pure function of `|dq4|` alone.** It clamps below 160 and
at or above 2032, so the entire non-clamped domain is 1,871 values and **a
direct 2,033-byte table is exact**, removing the two `k_tspf_invz` reads, the
difference, the multiply, the rounding and the shift. It fits a fixed bank with
no banking cost. **This one is worth doing.**

**`inv_at_invd` is `q1 = (invd*dot+64)>>7` then `q = (q1*sec+64)>>7`.**

| | reached | domain |
| --- | ---: | ---: |
| distinct `invd` | 221 | 256 |
| distinct `|dot|` | **50** | 128 |
| distinct `(invd,dot)` pairs | 7,216 | 32,768 |
| distinct `sec` values in the whole table | 54 | 513 indices |

Dense: `T1[invd][dot]` 32,768 bytes plus `T2[q1][secidx]` 13,824 plus a
513-byte `rel`→`secidx` map = **47,105 bytes**. Compacted through the 50 real
`dot` values and 54 `sec` values: about **27 KB**, but the 50 is a property of
THIS map's wall orientations and must be re-derived per map.

**The prize is small and that is the point.** Per update the corpus shows 4.29
`inv_for_dq4` calls and 8.58 `inv_at_invd` calls. At A13's measured 193 T per
multiply the whole projection multiply budget is roughly 4,000 T against a
242,103 T update — **about 1.7%**. The projection path is not where the time
is; the materializer is. So: take the 2 KB table, skip the 27-47 KB ones, and
do not bank-switch for 1.7%.

### Verdict

**The better abstraction is still "which screen columns changed".** The span
stream is a better thing to STORE and COMPARE — 3.74x smaller, exact, and it
proves a whole span unchanged in six bytes — but it is a worse thing to WORK
FROM, because the dirty region still has to be derived per column and the
stream makes that derivation more expensive, not less.

So: keep A29/A31's retained-column form, add the span record beside it as a
pre-check (UNION_E, −14.7% on mixed motion), and continue down the A33 path.
The identified 3x target there — UNION_D's restructure removing the
per-column presence test — is still unfinished and is still the thing to
finish.

### A33. The Z80 union, measured. It costs 35-55% of a full render and the tail exceeds it.

`make union-bench`. This is the go/no-go A31 named and A32 was optimistic
about, and the answer is negative in this form.

**What was built.** `tools/temporal_union.asm`, the cross-span dirty union as
real Z80, verified against the host classifier's own mask. The boundary probe
dumps, per pose pair, exactly what the kernel reads — the retained
3-byte-per-column state of both poses — and exactly what the host produces from
it as an 18x20-bit mask. **The kernel must produce a SUPERSET: conservative is
allowed, missing a cell is not**, because a missed cell is a wrong image. That
check is the bench's first act and it caught four separate defects (below).

**The retained state is (hl, hr, border), and NOT (il, ir).** Every endpoint
derives from `hl = il>>1`, so `il` and `il+1` across an even boundary give
identical geometry. Retaining `il` reports change where there is none:
**3,568,759 spurious columns in 73,521,834, 4.9%**, each one a column the union
would dirty for nothing. The host now asserts the equivalence of the 3-byte
compare and the full one on every column — 0 disagreements with the height
bytes. DDA_G already holds them as HLH/HRH.

**Measured, on the same U=1 corpora A30 used:**

| corpus | UNION_C mean | % of DDA_G's 153,450 T | p95 | p95 as % |
| --- | ---: | ---: | ---: | ---: |
| U=1, pure rotation | 84,307 T | **54.9%** | 202,489 | **132%** |
| U=1, all regimes | 53,282 T | **34.7%** | 189,292 | **123%** |

**The tail is the disqualifying part, not the mean.** DDA_G is a full render at
~153,450 T with low variance. On 5% of updates the union ALONE costs more than
a complete render — 132% of it — before a single cell is drawn or restored.
That is A30's topology population arriving exactly where it was predicted to,
and a renderer whose cheap path saves 45% while its 5% path loses 32% is not
obviously faster; it is differently shaped.

**The ladder, one mechanism per rung as A24/A26/A27 required:**

| variant | T/update | marks | over host | exact |
| --- | ---: | ---: | ---: | :-: |
| UNION_A — one conservative profile-free range per column | 82,955 | 249.81 | +249% | yes |
| UNION_B — the two EDGE ranges only, interior stays resident | 87,700 | 97.92 | +37% | yes |
| UNION_C — B plus table-driven marking | **84,400** | 97.92 | +37% | yes |
| UNION_D — no presence test in the inner loop | 84,372 | 273.07 | +281% | **NO** |

**UNION_A is the instructive failure.** `[71-hmax, 72+hmax]` contains the drawn
rows for every profile, so it needs no branch and is correct by construction —
and it marks 249.81 cells against the host's 71.65, because it marks whole
columns and so **throws away the interior-resident mechanism that is the entire
point of A29**. Correct, cheap-looking, and architecturally self-defeating.

**UNION_B fixes that and costs 5.7% MORE.** Marking only the two edge ranges
cuts the marks to 0.39x, and the extra profile branching eats the saving. The
interior's own boundary rows need no separate mark: the interior start is
`row_floor(max top)+1`, so it moves with the top edge and always lands inside
that edge's own old/new union — which is why the host attributed 0.00 cells to
interior enter/leave over 3.5M pose pairs.

**UNION_C is the rung that matters, and it returned 3.8%.** The profile put the
mark family at 33% — `mk_sh`'s bit-shift loop 6.3%, `mk_have`'s pointer
arithmetic 11.8% — so three tables (COLBIT, COLBYTE, ROWBASE) were supposed to
remove most of it. They removed 3.8%. **The profile share did not translate
because there are only ~40 marks per update**, so per-call savings are small
against the total. That is the third time in this project a profile share has
failed to convert (A24's ROWPTR_B, A27's re-profile, this).

**Where the time actually goes, re-profiled after the change as the standing
rule requires:** the per-column machinery, not the row extents.
`ub_o_done` + `ub_n_done` + `ub_one_col` + `ub_col` ≈ 18% and `ub_off` 6.4% —
all of it presence testing and iteration, none of it work. UNION_B sweeps the
UNION of both column ranges for every span and asks "present in new? in old?"
with four push/pop pairs and two calls, per column.

**So the diagnosis differs from A26 even though the verdict so far matches.**
A26 was expensive because deriving a column's row extent cost 1,975 T. Here the
extent is free — A32's counting was right about that — and the cost moved to
iterating and testing columns. Fixing it needs the loop restructured so the
inner loop contains no presence test at all: overlap, new-only, old-only as
three separate sweeps. **UNION_D is that attempt and it is NOT CORRECT** — 68
missed cells and 281% over-marking — so its 84,372 T is not a result, it is
what an incorrect kernel costs. It is left in the tree, clearly marked, as the
next thing to finish.

**Four defects the superset check caught**, each invisible to anything weaker:

1. **Only new spans were visited.** Vanished spans own cells that must be
   restored and nothing in pass 1 reaches them. 22,099 missed cells.
2. **Draw-order inversions ignored.** Two retained spans swapping rank changes
   the winner of any cell they share with neither span's own state moving —
   A31's failure mode. Detected for free by requiring the old slot indices to
   increase as new spans are walked in rank order.
3. **Marking only the span AT the inversion.** Every span it jumped over
   swapped with it too. Now handled once in a tail, conservatively.
4. **The border byte compared last.** A column whose height AND border both
   moved took the edges-only path, because the border flag only got set when
   the border happened to be the first differing byte. A border bit lives on
   the FULL interior tile, so the whole column is dirty. 151 missed cells, and
   the fix is to compare the border first.

Defect 2 fires on **11.2%** of rotating updates and costs 202,883 T against
73,189 T without, so it contributes ~14,500 T of the mean on its own.

**What this does NOT say.** It does not refute the mechanism. This is a first
implementation, and A24 took four rungs to get 22.9% out of code of exactly
this shape; UNION_D's target is identified and unfinished. What it does say is
concrete: **the union needs roughly a 3x implementation improvement to be worth
continuing, and the p95 needs a separate answer, because no amount of mean-case
work fixes a tail that exceeds a full render.**

**What closes A33:**
1. Finish UNION_D correctly — three sweeps, no inner-loop presence test — and
   re-profile. This is the identified 3x target.
2. Answer the tail separately. A30 already said the tail is topology and does
   not shrink with cadence. A full render is 153,450 T and bounded; the union's
   p95 is 202,489 T and is not. The sliced/deadline scheduler was deferred
   pending a CPU cost distribution — this IS that distribution, and it says
   the exceptional path needs a budget cap or a fall-back-to-full-render rule.
3. Only then TEMP_BOUNDARY_A. Building the executor on a union that costs half
   a render would be building on the wrong number.

### A32. Bounded-error temporal rendering — measured, and it does not pay. But the union is cheap.

`make temporal-error`. Three questions, three answers, one of them the opposite
of what was expected.

**Reduction test first.** `tools/temporal_error_probe.c` runs three independent
simulations, one per error budget, each with its own displayed state and name
table. **At E=0 the displayed image must equal the exact render on every
frame** — 0 mismatches, checked every invocation. Without that the approximate
path could be measuring its own bugs. The span-state model is now a shared
header, `tools/temporal_span_state.h`, included by both probes, because
duplicating it is exactly where an exact/approximate drift would hide; the
exact probe's output is byte-identical after the refactor.

**1. Bounded error buys little, and the reason is not what it looks like.**
Savings are measured against the EXACT temporal path (A29/A31), never against
the full render — measuring against the full render would re-bank what A29
already banked.

| corpus | E=1 | E=2 | wrong cells/frame | worst frame |
| --- | ---: | ---: | ---: | ---: |
| rotation, U=1 | **87.5%** | 80.9% | 9.3 | 88 |
| rotation, U=2 | 93.9% | 89.5% | 5.1 | 52 |
| rotation, U=4 | 97.5% | 95.1% | 2.2 | 30 |
| all regimes, U=1 | 84.3% | 76.1% | 6.8 | 88 |

So 12.5% off the exact path at 1 px under rotation at U=1, 15.7% across all
regimes, and it gets *worse* with cadence — 2.5% at U=4 — because a bigger
per-update delta rarely fits inside a 1 px budget.

**The obvious explanation is wrong and was checked.** One would guess the
saving is small because deferral mostly suppresses geometry changes that
produce no tile change, which A29 already gets for free. It does not:
**87.8% of deferred columns at U=1 (92.0% at U=4) would have changed a tile.**
Deferral is suppressing real work. The saving is small because the dirty set is
dominated by things that are never deferrable — `V_COLUMN_SHIFT`, columns
entering or leaving a span, and topology, all held exact by construction per
A31's finding that ownership errors look almost right.

**The bound holds and convergence is one frame.** Measured maximum error equals
the budget exactly, 1 and 2 px. Convergence needed an explicit clause — when
the target has not moved since the previous update, adopt it exactly — and with
it, settling after motion stops is **0 frames**: the first stationary update is
exact. Without that clause a within-budget error would persist forever, which
is a silent permanent wrongness rather than a lag.

**Verdict: do not build the bounded-error path.** 12–16% off the exact path, in
exchange for 7–9 visibly wrong cells per frame with peaks near 90, an error
accumulator per boundary, and a second code path that must converge correctly.
The exact path is both cheaper to reason about and nearly as fast.

**2. H-scroll as a first-order yaw approximation — better than A28 measured,
still not enough.** A28 tested a whole-COLUMN shift against EXACT matching and
got 0.6%. A per-PIXEL scroll with a tolerance is a different question and does
much better on the horizontal axis:

| | rotation U=1 | all regimes U=1 |
| --- | ---: | ---: |
| mean residual after the best single scroll | 2.34 px | 1.53 px |
| endpoints within 1 px | **73.1%** | 87.3% |

But a scroll register moves the image horizontally and does nothing to heights,
and the secant term A28 identified rescales every height under yaw. That half:

| vertical boundary motion, same column | rotation U=1 | all regimes U=1 |
| --- | ---: | ---: |
| mean | 1.76 px | 1.02 px |
| within 1 px | **64.5%** | 78.7% |
| max | 23 px | 94 px |

A column is only reusable if BOTH axes are inside budget, so under rotation
roughly **47%** of columns qualify — and the scroll displaces the whole image
including the 27% of endpoints outside budget, which become wrong by up to
12 px. It is a trade, not a free win, and a worse one than the plain
bounded-error path above.

**3. The important positive: the mandatory cross-span union is CHEAP.** A31
proved the union is required and flagged its cost as "the whole question",
because A26 died on a classifier costing 1,975 T per column. Counted:

| union work, per update | all regimes U=1 | rotation U=1 |
| --- | ---: | ---: |
| 6-byte state comparisons | 22.30 | 31.69 |
| row-range marks into the masks | 33.07 | 83.63 |
| rows containing any dirty cell | 5.85 of 18 | 14.61 of 18 |

**This is not A26's shape at all.** A26's classifier was expensive because
knowing a column's row extent was expensive; here the extent comes from
retained state and costs nothing to look up. The union is a couple of dozen
six-byte compares over the whole update, not per column. **A31's blocker is
much smaller than A31 feared** — still to be confirmed in Z80, but the
operation count is now known rather than assumed.

**4. Row/column summary masks help, unevenly.** The viewport is 20x18, so a row
mask is 20 bits and an 18-bit summary says which rows contain anything. Cells
visited when scanning the dirty set:

| | flat 360-cell scan | with the 18-bit row summary |
| --- | ---: | ---: |
| all regimes U=1 | 360 | **117 (33%)** |
| rotation U=1 | 360 | 292 (81%) |

Worth having, and worth nothing under rotation, where 14.6 of 18 rows are dirty
anyway. Size the expectation to the regime.

**5. The three VDP-ignored name-table bits (13-15): a clear loss for
restoration, on maintenance cost.** They are genuinely free — the renderer uses
tile id bits 0-8 plus FLIPX/FLIPY/PALETTE in 9-11, and nothing reads 13-15.
But:

| | all regimes U=1 | rotation U=1 |
| --- | ---: | ---: |
| cells whose semantic class changes | **9.27** /update | 21.15 /update |
| dirty cells the union already produces | 29.21 | 74.91 |

Maintaining the tag costs about **32%** more cell writes on top of the dirty
set. What it buys on restoration is narrow by construction: **3 bits cannot
name which of up to 20 spans lies behind a cell**, so the most it can do is
flag "background is behind, do not scan", and A30 puts that at 21.9% of the
8.03 vacated cells — about **1.8 skipped scans per update**. Paying 9.27 writes
to save 1.8 shallow scans is a loss, and it is the A26 trap in miniature:
maintaining the metadata costs more than consulting it saves.

**If it is ever revisited**, the caveat the user raised is real and belongs
here: with metadata in bits 13-15, every visual equality test and dirty compare
must mask them, or a metadata-only change triggers a VRAM write. Semantic
comparisons keep them; visual ones must not.

**Where this leaves the plan.** Build `TEMP_BOUNDARY_A` exact, as A31 laid out.
Bounded error is closed — measured, bound verified, convergence verified, and
the saving is too small for the visual cost. The scroll variant is closed with
it. The 3-bit tag is closed on maintenance cost. What is NOT closed and just
got much more promising is the exact union itself.

### A31. The Z80 A/B foundation — sequence oracle verified, and the obvious rung is WRONG.

Two deliverables, and the second is the more valuable one.

**1. A pose-SEQUENCE oracle, and DDA_G reproduces it exactly.**
`make temporal-bench`. Every materializer bench so far verifies one pose
against `coverage_pose_oracle.txt`. A temporal kernel carries state ACROSS
poses, so a single-pose oracle cannot see the mechanism at all — the same gap
A26 had to close when coverage state started crossing runs. The unit of
verification is now a consecutive sequence of poses along a real trajectory,
emitted by `temporal_boundary_probe.c` at a chosen cadence, in the same 8-field
run format plus a 9th field, the key id, because a temporal kernel has to match
a span to its retained state and draw order alone does not identify it.

Before anything is built on it, the oracle itself is proved:
`tools/z80_temporal_bench.py` renders every pose from scratch with the
**unmodified DDA_G** and hard-fails unless the dumped name table matches.

    sequence oracle: 5 trajectories, 300 poses, 295 consecutive pairs
    ORACLE VERIFIED: DDA_G reproduces all 300 poses exactly
    BASELINE_DDA   153,449.8 T/update   26.57 columns/update

(That baseline is lower than the 194,761 whole-corpus figure because this
sample is five rotation trajectories, not the whole map. It is the A/B's own
reference, not a new budget line.)

**2. The obvious first rung is INCORRECT, measured before writing it.**

The natural cheap kernel is a purely local test: *if this span's column state is
unchanged, skip the column.* No cross-span union, no second pass, no dirty set —
which is attractive precisely because the union is what made A26's classifier
cost 58,073 T against a 24,100 T saving.

It does not work. Simulated exactly, starting from the previous name table and
redrawing far->near only the columns whose own state changed:

| corpus | pose pairs wrong | cells wrong /update |
| --- | ---: | ---: |
| all regimes, U=1 | **30.9%** | 3.05 |
| pure rotation, U=1 | **76.7%** | 5.35 |
| all regimes, U=4 | 33.8% | 8.08 |
| pure rotation, U=4 | **82.7%** | 15.71 |

**Why, precisely.** In far->near order an unchanged span writes the same words
it wrote last update — but a NEARER span may have moved away, uncovering cells
the unchanged span owns. Skipping it leaves the departed near span's stale
pixels there. The span's own state being unchanged says nothing about whether
its cells are still covered.

**Note the shape of the failure**, because it is the dangerous kind: only 0.85%
to 4.4% of cells are wrong, on 77% of frames. It would look almost right. A
frame-average test, a screenshot, or a spot check would pass it. Only a
cell-exact oracle on a sequence catches it, which is the third time this
project's exactness rule has paid (A26's two pose-scope bugs, A29's
`map_init`/`g_touched_bits` overrun, this).

**What this forces on the kernel design.** `TEMP_BOUNDARY_A` must build the
cross-span dirty union before drawing, i.e. two passes: classify every span's
boundary events and mark cells, then draw only marked cells. That is precisely
the shape whose cost killed A26, so **the classifier cost is the whole
question** and it must be measured, not assumed. **A32 has since counted it and
it is small** — 22.30 state comparisons and 33.07 row-range marks per update,
because the row extent comes from retained state instead of being re-derived,
which is exactly what made A26's classifier expensive. The budget it has to beat is
concrete this time: A30 says an ordinary update at U=1 has 29.2 dirty cells and
10.93 temporal ops against a baseline of 26.57 column-materializations.

**What is NOT yet built:** the temporal kernel itself. The harness, the
sequence oracle and the verified baseline exist; `TEMP_BOUNDARY_A` does not.
No T-state figure is claimed for it.

**Rungs, in the order the measurements justify** — one mechanism each, pose-
sequence-exact, re-profiled after every rung as A24/A27 required:

1. `TEMP_BOUNDARY_A` — persistent map, retained per-column `(il, ir, border)`,
   cross-span dirty union, skip unchanged columns, keep the interior resident
   on edge-only columns. **The interior-resident path is the valuable half**:
   A30 puts full skipping at only 13.1% of columns under rotation at U=1 while
   edge-only is 61.1%.
2. `TEMP_BOUNDARY_B` — `V_COLUMN_SHIFT` as a run operation. A30 shows 97.3% of
   spans move zero or one column at U=1, so this is `SHIFT_RUN dx=+/-1`.
3. `TEMP_BOUNDARY_C` — `tile_id += delta` for phase and slope. Reachable on
   53.7% of edge transitions under rotation at U=1, guarded by the attribute
   bits and the `off`/`mag` clamps.
4. `TEMP_BOUNDARY_D` — grow/shrink at the tips only.
5. `TEMP_BOUNDARY_E` — restoration. A30 says a near->far scan over retained
   state at mean depth ~3, not a baked token and not an underlay cache.

### A30. Cadence sweep — the workload is a feedback loop, and V_COLUMN_SHIFT collapses.

`make temporal-cadence` reruns A29's verified boundary representation at U=1
through U=8. **The go/no-go and self-check stay clean at every cadence**: 0
cells changed outside the dirty set, 0 state-only re-derivation mismatches.

**Why this had to be measured before designing anything.** A29's corpus ran at
U=4 because that is what a 194,761 T materializer can afford — about 15 unique
updates/sec. Designing the temporal renderer against that workload bakes in the
old renderer's speed. The optimization is a loop: faster renderer, smaller
camera delta, fewer boundaries crossed, fewer events, faster again. At the
shipped turn rate of 3 yaw units/tick, U=1..4 is dyaw 3, 6, 9, 12, or 4.22 to
16.88 degrees.

**Pure rotation** — the case that decides everything:

| U | dyaw | deg | dirty% | floor% | skip% | edge% | full% | V_COL% | vacated | tile_id+= |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3 | 4.22 | **27.2** | 25.8 | 13.1 | **61.1** | 25.8 | **16.0** | 20.6 | **53.7%** |
| 2 | 6 | 8.44 | 35.8 | 31.6 | 4.9 | 59.3 | 35.9 | 28.2 | 39.8 | 44.2% |
| 3 | 9 | 12.66 | 42.4 | 35.1 | 2.4 | 54.8 | 42.8 | 37.6 | 57.7 | 36.4% |
| 4 | 12 | 16.88 | 48.2 | 38.1 | 1.2 | 49.9 | 48.9 | 45.0 | 74.2 | 30.1% |
| 8 | 24 | 33.75 | 66.8 | 47.3 | 0.1 | 33.8 | 66.0 | 61.1 | 126.5 | 13.7% |

**All regimes:**

| U | dirty% | floor% | skip% | edge% | full% | V_COL% | vacated | tile_id+= |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | **12.6** | 11.7 | **52.3** | 36.9 | 10.8 | **6.8** | 8.0 | **61.5%** |
| 2 | 17.1 | 15.2 | 46.0 | 38.3 | 15.7 | 12.4 | 15.2 | 56.9% |
| 4 | 23.8 | 19.4 | 41.7 | 35.8 | 22.5 | 20.9 | 28.2 | 51.7% |
| 8 | 33.8 | 25.5 | 39.0 | 29.1 | 31.9 | 30.1 | 49.1 | 50.6% |

**1. V_COLUMN_SHIFT collapses, as predicted.** 45.0% of rotation events at U=4,
**16.0% at U=1**. And the shape of what remains is the important part — span
slide per update, pure rotation:

| U | 0 columns | 1 column | 2 | 3 | 4 | 5 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 39.9% | **57.4%** | 2.7% | — | — | — |
| 2 | 28.6% | 20.9% | 46.8% | 3.7% | — | — |
| 4 | 24.4% | 2.3% | 3.0% | 40.9% | 26.7% | 2.6% |

At U=1, **97.3% of spans move zero or one column.** That is precisely the
`SHIFT_RUN dx=+/-1` case, not an N-column rebuild. At U=4 the mode is a 3- or
4-column jump, which is why it looked like an area problem there. It was an
artefact of the old renderer's cadence.

**2. The `tile_id += delta` fast path improves with cadence too**, 30.1% to
53.7% under rotation and 51.7% to 61.5% overall, because the phase deltas that
fall outside the increment are the large ones.

**3. A negative result that constrains the design: one shared delta per span
does NOT get better with cadence.** Whether a whole span's four boundaries can
be advanced by a single `(dtl,dtr,dbl,dbr)` sits at 16-19% under rotation at
EVERY cadence, and 52-58% across all regimes. So:

> **The answer to A29's open question is "it depends on the motion".** Under
> translation, roughly half of spans can have their whole boundary state
> advanced by one shared delta, so an edge-only column escapes its geometry
> derivation. Under rotation it is one in six, because the secant term A28
> identified varies across the span, so most edge-only columns still need
> per-column geometry and only escape the interior writes. Any T projection
> that assumes the first case is wrong for turning, which is the expensive case.

**4. Restoration — the blocker, now measured rather than described.** At U=1
under rotation, 20.6 vacated cells/update:

| what replaces it | share |
| --- | ---: |
| background (ceiling/floor/horizon) | 21.9% |
| a span ALREADY in retained state | 74.0% |
| a span that appeared this update | 4.1% |

Two proposed mechanisms are **refuted by this**:

- *A one-deep "second owner" underlay.* Only **5.1%** of known-span restores
  had that span already contributing the same word underneath last update.
  Everything moved; the underlay is stale exactly when it is needed.
- *A "next owner" pointer.* The new owner is the span immediately behind the
  old one only **27.5%** of the time.

What the data DOES support is a near->far scan over retained span state, depth
1:26.6%, 2:24.9%, 3:16.5%, 4:12.6%, rest 19.4% — **mean about 3**. With 4.24
visible spans under rotation that is a short linear scan over state already
held, not a coverage bitmap, and A25/A26's verdict against generic runtime
coverage does not apply to it. Cost per test is the same interior/bottom/top
range check `contrib` does. **20.6 cells x ~3 tests per update is the number to
beat, and it is not yet costed on the Z80.**

**5. The tail is topology, not motion rate, and cadence does not fix it.**
Under rotation the max dirty falls 357 -> 223 from U=4 to U=1, but across all
regimes the max stays pinned at **360, a whole screen, at every cadence**,
while the mean falls from 54.8 to 29.2. That worst case is spans appearing and
vanishing — doorway crossings — at 0.212 and 0.235 per update. Rendering faster
does not make a doorway crossing cheaper.

> **Consequence for the scheduler.** The deadline-driven or sliced update has to
> be designed around TOPOLOGY events specifically, not around a generic
> percentile. An ordinary update is 29 cells; a doorway crossing is up to 360
> and no cadence increase reduces it. That is a two-population distribution, and
> a budget cap that treats it as one will either be too slow for the common case
> or blow the deadline on the rare one.

**What this does NOT establish.** No T-state figure, and in particular the
dirty-cell percentage is still not the T percentage — the interior fill it
removes is ~7.9% of A27's profile. The fixed-point calculation the user asked
for (measured cost at cadence X -> achievable cadence Y -> event distribution
at Y -> revised cost) now has its first table, but it needs a measured Z80 cost
to iterate on, and this project has been wrong on five straight estimates.

**What closes A30:** the Z80 A/B. `TEMP_BOUNDARY_A` against `DDA_G`, pose-exact,
one mechanism at a time as A24/A26/A27 were done, costed against the **U=1 and
U=2 event distributions** above rather than U=4.

### A29. Temporal BOUNDARY events — the representation is complete, and it works under rotation.

`make temporal-boundary` builds `tools/temporal_boundary_probe.c` on the same
corpus A28 used, so the two are directly comparable: 1,864 spawns, 879,808
update pairs, U=4. **Perspective projection unchanged.**

**What is retained, and why exactly this.** The renderer's per-column output is
a pure function of six values — `(tl, tr, bl, br, shade, border)` — through
`draw_edge`(top), `draw_edge`(bottom), `draw_full`(interior) in that order,
interior last so it wins on overlap. Nothing else about the projection can
reach a cell. That sextuple IS the retained boundary state.

**Two checks run on every pose pair, and both are load-bearing:**

1. *Self-check.* Re-derive the whole 20x18 name table from span state alone and
   require it to equal the renderer's own output. **0 mismatches.** If the
   retained state were missing an input, every number below would be noise.
2. *Go/no-go.* Every cell OUTSIDE the classifier's dirty set must already hold
   the correct new word. **0 misses, 0/879,808 pairs.** The representation
   describes every visual change; it is not a heuristic that mostly works.

**The classifier reads retained state only.** It never renders a cell and
compares — that would only prove stores are skippable, which A28 already showed
and which is not the point. Its rules: column present in one pose only marks
all its rows; shade or border change marks the column; a moved top or bottom
edge marks the union of old and new edge rows; and the interior contributes
only its **symmetric difference**. That last line is the entire hypothesis:
rows that were interior and still are, at the same shade and border, are not
dirty however far the geometry moved.

| regime | dirty % of row-writes | col skip | col edge-only | col full |
| --- | ---: | ---: | ---: | ---: |
| stand still | 0.0% | 100.0% | 0.0% | 0.0% |
| strafe | 8.1% | 81.8% | 10.9% | 7.4% |
| walk forward | 8.3% | 64.5% | 30.9% | 4.6% |
| walk, rare nudge | 13.0% | 53.7% | 37.4% | 8.9% |
| walk, nudge turn | 24.4% | 24.5% | 59.5% | 16.0% |
| demo path (shipped) | 39.2% | 11.5% | 64.1% | 24.4% |
| turn in place | **48.2%** | 1.2% | 49.9% | 48.9% |
| walk + turn | 50.4% | 1.1% | 47.9% | 51.1% |

**Against A28, on the identical corpus and the identical rotation:**

| metric, pure rotation | A28 | A29 |
| --- | ---: | ---: |
| whole `(run,column)` key survives | 0.1% | 1.2% |
| columns needing only boundary work | not measured | **49.9%** |
| row-writes eliminated | not measured | **51.8%** |

A28's number was not wrong, it was answering a question that does not gate the
architecture. The finer mechanism survives rotation; the coarse one does not.

**All regimes:** 230.54 row-writes/update now, **54.84 dirty (23.8%)**, against
an exact floor of 44.80 (19.4%). The classifier overshoots that floor by
**1.22x**, so the cheap boundary test is close to the best any test could do.
Interior cells left resident: 69.77/update. Column-materializations 21.39, of
which 41.7% fully skippable, 35.8% edge-only, 22.5% needing full derivation.

**The tail is real and must not be averaged away.** Dirty cells mean 54.84, p95
**194**, max **360** — a whole screen. A kernel that is fast on the mean and
falls back on the p95 has a frame-time distribution, not a frame time. Whatever
gets built needs the deadline-driven or sliced upload the user raised, or a
budget cap, not just a good average.

**Event classes, empty-column non-events excluded** (counting a column neither
pose touches as NO_CHANGE inflated this to 71.7%, which was mostly the screen's
blank space congratulating itself):

| class | all regimes | pure rotation |
| --- | ---: | ---: |
| NO_CHANGE | 37.4% | 0.9% |
| PHASE_SHIFT | 4.5% | 1.4% |
| PHASE_RAMP | 0.4% | 0.1% |
| EDGE_CROSS | 8.5% | 5.4% |
| SLOPE_CHANGE | 18.8% | 31.2% |
| V_COLUMN_SHIFT | 20.9% | **45.0%** |
| FALLBACK | 9.5% | 16.0% |

Vertical grow 2.31/update, shrink 1.41/update. These are ATTRIBUTES, not
exclusive classes — a column can ramp its top edge and grow downward in the
same update — and counting them only when nothing else fired reported them as
0.0% and hid a real 7.41 cells/update.

**`V_COLUMN_SHIFT` is now the largest single item under rotation and it is the
next thing to attack.** A run sliding 3-4 columns per update acquires that many
wholly-new columns and vacates as many. But a newly covered column of the same
wall is nearly its neighbour: same shade, usually the same border, edges one
slope-step along. Encoding it as a run operation rather than N independent
column derivations is the obvious lead and is NOT measured.

**A30 answers the open question below and supersedes the U=4 numbers in this
entry.** The workload is cadence-dependent: at U=1 the rotation figures become
27.2% dirty, 61.1% edge-only and 16.0% V_COLUMN_SHIFT. Read A30 before using
any number here for design.

**The edge LUT is already laid out as a temporal state machine, half the time.**
`TSP_TILE_EDGE = BASE + ((shade*16 + off)*8) + slope`, so within-cell vertical
phase has stride **8** and quantized slope has stride **1**, both constant.
Measured over 6,335,103 edge-cell transitions: attribute bits (FLIPX/FLIPY/
PALETTE) match on **90.4%**, and **51.7%** are reachable by `tile_id += small
delta`. Phase deltas cluster where the user predicted: −1 at 15.0%, −2 at 8.7%,
+1 at 5.9%, +2 at 3.1%. The other half is blocked by the attribute bits, which
`edge_entry` derives from the slope's sign, and by the `off`/`mag` clamps. So
the increment is a real fast path with a real guard condition, not a universal
one.

**Baked-transition entropy:** 8,061,356 per-column boundary deltas observed,
**1,426 distinct `(dtl,dtr,dbl,dbr)` codes** clamped to +/-8. A small alphabet,
which is what makes a transition table bakeable. This counts geometry deltas
only, not the cell programs they expand into, so it is indicative and not a ROM
figure.

**The restoration problem, stated as the next specific blocker rather than
hidden.** 6.60 cells or column-runs per update are vacated by a shrinking or
departing boundary. Their correct new content is background, a FULL interior of
the same span, or another farther span. This probe recomputes them from the
full run list, which a runtime cannot afford. **The Z80 cost of deciding what
goes back into a vacated cell is not measured and there is no candidate cheap
mechanism yet.** Topology events per update: 0.212 spans appearing, 0.235
vanishing, 0.320 draw-order flips.

**A bug the ownership rule caught in itself.** The first draft marked every
cell of both spans when two spans swapped draw order. That cost 48.6% of the
dirty set under rotation, nearly all of it cells only one span ever touched.
Restricting the mark to cells both spans cover dropped the dirty set from
165.96 to 131.84 per update with the go/no-go still clean. Order flips are rare
(0.320/update) but marking them wrong is expensive.

**What is explicitly NOT claimed.** None of this is a T-state figure. The
row-write reduction is not the T reduction: A27's profile puts the interior
fill (`df_loop`) at ~7.9%, so the cells this removes are the cheap ones, while
an edge-only column still needs its geometry derived unless the retained-state
delta path replaces that derivation too. **That is the question the Z80 A/B has
to answer, and this project has been wrong on five straight estimates, so no
number is written here.**

**What closes A29:**
1. Attack `V_COLUMN_SHIFT` as a run operation and re-measure. Largest item
   under rotation.
2. Decide the vacated-cell restoration mechanism. It is the blocker, not a
   detail.
3. Only then build a Z80 executor, costed from this project's own MEASURED
   kernel costs, and prove it with an A/B twin against DDA_G at pose scope,
   exactly as A24/A26/A27 were proven.

### A28. Temporal delta — measured before building, and rotation kills it.

`make temporal-delta` builds `tools/temporal_delta_probe.c`. It drives the real
motion model (`tsp_step`, the shipped one) along eight motion regimes crossed
with every walkable spawn cell and four yaws — 1,864 spawns, 879,808 update
pairs at U=4 — and asks what a temporal skip could actually eliminate.

**The measurement unit matters and is stated first.** The materializer's unit
of work is one (run, screen column), and that unit's output is a pure function
of `K = (profile, il, ir, border, shade)` where `il = clamp((jq+32)>>6)`. So the
probe compares work KEYS, not pixels. It also asserts the invariant that an
identical ordered run list must produce identical cells; that assertion fired
(9 violations) the moment a scratch-column draw was added, because
`map_init()` on the scratch cleared `g_touched_bits` while `g_touched_count`
kept counting, so later `mark_touched` calls re-appended cells and overran
`g_touched_list[360]` into neighbouring statics. Saving and restoring the whole
bookkeeping fixed it. **Every number below is from a run with 0 violations.**

| regime | mean dyaw | cells same | work skippable |
| --- | ---: | ---: | ---: |
| stand still | 0.00 | 100.0% | **100.0%** |
| strafe | 0.00 | 96.0% | **74.3%** |
| walk forward | 0.00 | 95.1% | **59.7%** |
| walk, rare nudge | 1.47 | 92.7% | 48.8% |
| walk, nudge turn | 3.00 | 86.1% | 18.6% |
| demo path (shipped) | 2.62 | 89.0% | 3.3% |
| turn in place | 12.00 | 70.8% | **0.1%** |
| walk + turn | 12.00 | 70.6% | **0.1%** |

**Rotation is the entire story, and it is not a matter of degree.** Bucketed by
per-update yaw delta, work skippable is 64.3% at dyaw 0 and then falls off a
cliff: 0.8% at dyaw 1–2, 0.7% at 3–4, 0.5% at 5–8, 0.1% at 9–16. There is no
gentle slope. **One yaw unit of rotation — 1/256 of a turn — destroys the
temporal delta almost completely.**

The motion model turns at 3 yaw units per frame, and the materializer's
194,761 T means an update lands about every 4th frame, so the real per-update
rotation while turning is 12 units. Sweeping the update period does not rescue
it: even at U=1, an update every single frame and four times faster than
anything achievable today, turn-in-place still only reaches 3.7%.

| U (frames/update) | all regimes | turn in place |
| ---: | ---: | ---: |
| 1 | 43.6% | 3.7% |
| 2 | 40.1% | 0.9% |
| 4 | 38.3% | 0.1% |
| 8 | 37.4% | 0.0% |

**Why rotation destroys it — mechanism, not speculation.** Under pure rotation
a wall's corners keep exactly the range they had, so the naive expectation is
that projected depth is invariant. It is not: `inv0`/`inv1` are unchanged on
only **1.0%** of matched runs. Splitting by endpoint kind rules out FOV
clipping as the cause — runs with BOTH endpoints real corners sit at 1.2%,
clipped ones at 0.9%.

The cause is in the shipped code. `inv_at_invd`
(`src/tilesector_polar_renderer.c:378-386`) ends with

```c
sec = k_tspf_sec_q7[|rel|];  q = (q*sec+64u)>>7;
```

and `rel` is the bearing **relative to yaw** (`st = signed_q12(a0 - yawq)`).
`k_tspf_sec_q7` is a true secant of that screen-relative bearing, verified
against `128*sec(theta)`: 128/128 at 0°, 139/138.5 at 22.5°, 181/181 at 45°.
So the projected depth carries an explicit `sec(bearing - yaw)` factor.
Rotating the camera rescales every run's projected height even though nothing
in the world moved. That is a property of RECTILINEAR projection, not a bug.

**Hardware H-scroll cannot absorb it, and that was measured too.** The probe
searches every whole-column shift and takes the best. Under rotation this moves
work skipped from 0.1% to **0.6%** — nothing. The reason is the same tangent
mapping: `k_tspf_angle_x_pos` is exactly `80 + 80*tan(theta)` at 90° FOV, so a
yaw step shifts the screen centre and the screen edge by different amounts.

| dyaw | shift at centre | shift at edge | spread |
| ---: | ---: | ---: | ---: |
| 1 | 2 px | 4 px | 2 px |
| 4 | 8 px | 14 px | 6 px |
| 12 (the real rate) | 24 px | 37 px | **13 px** |

A rigid scroll can absorb a rotation only if that spread is zero. It is zero
under a CYLINDRICAL mapping and only under a cylindrical mapping.

**Row-write level, for direct comparison with A25's spatial 9.1%.** Of the
230.54 row-writes per update, **77.5% already carry the word that is being
stored** (59% even under pure rotation). This is a genuinely large number and
it is NOT the same thing as the work figure: it bounds STORE elimination only.
The gap exists because the tile vocabulary is coarse — a column's tile survives
geometry changes that its derivation inputs do not — and A24 already showed the
stores are the cheap part (`FILLLOOP_D` left the two stores as the only memory
traffic in the fill loop). Knowing which stores to skip still costs the
derivation. **This is exactly the shape of the A26 trap** (classifier 58,073 T
against a 24,100 T saving) and must not be quoted as a saving.

**Cross-validated against an independent probe.** `polar_transition_bake.c`
already existed and had never been recorded here, in violation of this file's
own rule 1. Run at full resolution (466 nodes x 256 yaws) it reports `turn±`
changing 38.29 of 360 words per SINGLE yaw unit — 10.6% — against this probe's
independently measured 8.3% cell change at dyaw 1–2. Two probes built from
different directions agreeing within a couple of points is evidence the
workload model is faithful.

**And it prices the compiled-transition architecture, which is the finding that
matters most for planning.** That bake emits **143,217,187 bytes — 136.58
MiB** — for a coarse cell-centre state graph, against a 128 KiB ROM target.
That is roughly **1,090x over budget**, and it is the *optimistic* case: cell
centres only, no sub-cell offsets. Any "bake the transitions" proposal starts
from that number.

**Verdict — SUPERSEDED BY A29. Read that before acting on anything here.**

The measurements above stand. The conclusion drawn from them did not, and the
error is worth stating precisely because it is a repeatable kind of mistake.

This entry measured whether a whole `(run, column)` work key survived an
update, found 0.1% under rotation, and concluded the temporal direction was
gated on cylindrical projection. That is the right answer to a question nobody
needed answered. **A whole-key match is not the condition for skipping work.**
A wall column is a top edge of a few cells, a bottom edge of a few cells, and
an interior of identical FULL tiles. Move the geometry a pixel and every one of
`il`, `ir`, the endpoints and the slope changes, so the key never matches — but
the interior tiles are bit-identical and only the boundary cells can differ.

The tell was sitting inside this entry the whole time: **70.8% of final cells
unchanged under the same rotation that gave 0.1% key stability.** A 700x gap
between two metrics of the same phenomenon is a statement about the metric, not
about the phenomenon, and it should have been chased before a verdict was
written. A29 chases it and finds 51.8% of row-writes eliminable under pure
rotation, with the perspective projection untouched.

**The A6 promotion below is therefore withdrawn as a gate.** Cylindrical
projection remains a legitimate optional comparison — the best case if yaw were
a rigid screen shift — and the tangent/secant measurements above are the honest
statement of what it would buy. It is not a prerequisite, and the visual target
stays the existing perspective-projected viewport.

**What survives from this entry, unchanged and still useful:**
- The mechanism. `inv_at_invd` carries an explicit `sec(bearing - yaw)`, so
  yaw really does rescale every run's projected depth. A29 does not dispute
  this; it shows the rescaling mostly fails to reach the screen.
- H-scroll cannot absorb yaw under this projection. Measured twice.
- `polar_transition_bake.c` emits 136.58 MiB. Any pose-enumerating scheme
  starts from that number.
- The row-write figure, 77.5% of stores already correct, which A29 reinterprets
  rather than discards.

### A27. DDA_G — row extents by walking, not dividing. −12.0%, and it settles coverage.

`make dda-bench` verifies at pose scope, against the same oracle A26 used.

| variant | T/update | vs baseline | bytes |
| --- | ---: | ---: | ---: |
| FILLLOOP_D (A24 ladder end) | 221,229 | — | 1,154 |
| DDA_G | 194,761 | **−12.0%** | 1,293 |
| MASKED_H (DDA + coverage) | 238,338 | +7.7% | 1,698 |

**What DDA_G removes.** Two kinds of recomputation, both of something already
known:

1. **Row extents, six times per column.** Both edges and the interior each ran
   two signed 16-bit compares and two `rowfloor` calls. Every endpoint is a
   monotonic function of ONE height byte, so one byte compare per column picks
   the min/max ends and the rows follow by three shifts. `col_bounds` does it
   once and hands `draw_edge`/`draw_full` their bounds.
2. **The sub-row offset.** `local_left = YL - (r<<3)` was rebuilt every row
   from the row index. It is an affine walk — each row is exactly 8 less — so
   it is carried and decremented. That is the DDA proper.

`rowfloor`'s negative branch disappears entirely. A negative lower bound
clamps to row 0 and a negative upper bound means nothing to draw, so the shift
only ever sees a non-negative value and the negate/add-7/negate path is dead
code.

**The trap this hit twice** (both caught by the oracle, both the same shape):
a value that is *large* is not a value that is *negative*. `72+h` reaches 199
and `72+h-(h>>2)` reaches 168, so those shift as unsigned; testing bit 7 on
them reads 168 as negative and blanks the column. That is the identical trap
that bit RAISED in the per-column kernel. A second instance: `TOPR0` is
overwritten with a sentinel when the whole top edge is above the screen, and
the coverage mask must not see that sentinel — the column still draws its
bottom edge from row 0 — so the true lower bound is kept separately.

**MASKED_H settles A26 unconditionally.** A26's verdict on coverage was
explicitly conditional: the classifier was expensive because knowing a
column's rows was expensive, and DDA was supposed to make that free. It now
IS free — `col_bounds` already has the extent, so MASKED_H's classifier just
reads it — and coverage still costs **+22.4% over DDA_G**. The overhead was
never the extent. It is the mask machinery itself: the range/owned table
lookups, the three-byte accumulate, `cov_mark`, and the stash/restore on
partial columns, together ~1,400 T per column against a ceiling of ~800.

**Coverage at column granularity is closed.** Not deferred, not "revisit
later" — measured twice, on two different kernels, image-exact both times, and
losing both times. The remaining idea in this family is temporal, not spatial:
skip cells that did not change between updates. That is a different mechanism
with a different ceiling and it has not been measured.

**Re-profiled after the change, as the standing rule requires** — and the
ranking did NOT hold. The edge row-derivation that dominated before is gone,
and what is left is flat: `df_loop` 7.9%, `bd_done` 7.2%, `pf_done` 6.8%,
`de_loop` 6.4%, `row_addr` 5.8%, `ee_hi_ok` 5.5%. No single routine is above
8%. The largest coherent group is now the `edge_entry` clamp family
(`ee_hi_ok` + `edge_entry` + `ee_mag2` + `ee_lo_ok` + `de_sl_lo`) at ~18.6%,
which is clamping done with generic 16-bit compares on values that are known
to be small.

**Budget after DDA_G:**

| stage | T/update |
| --- | ---: |
| bearing | 5,833 |
| decode-clip | 11,036 |
| GATE | 2,339 |
| column-solve | 26,820 |
| depth sort | 1,314 |
| materialize (DDA_G) | 194,761 |
| **whole update** | **242,103** |
| **updates/frame** | **0.25** |

### A26. The masked kernel — built, exact, and it LOSES. Coverage does not pay yet.

`make masked-bench` builds the near->far coverage kernel A25 said was
available, and verifies it at POSE scope: every run of a pose, in near->far
order, against the final 20x18 name table. A run-scoped oracle cannot check
coverage — the mechanism *is* state carried across runs — so
`coverage_potential_probe.c` now also dumps `build/coverage_pose_oracle.txt`,
2,486 poses of run lists plus final images.

All four variants reproduce the name table exactly on 311/311 poses.

| variant | T/update | vs shipped | bytes |
| --- | ---: | ---: | ---: |
| FAR_NEAR_D (ships today) | 221,229 | — | 1,154 |
| PREP_ONLY (classifier only, no skip/stash) | 279,302 | +26.3% | 1,619 |
| MASKED_E (coverage, geometric classifier) | 277,264 | +25.3% | 1,614 |
| MASKED_F (coverage, cheap classifier) | 264,840 | +19.7% | 1,670 |

**The classifier is the entire story.** PREP_ONLY runs the column classifier
and then throws its verdict away, drawing every column unmasked far->near, so
it still has to produce the right image and it does. It costs +58,073 T. Set
against that, everything coverage actually buys — skipping 10.9% of columns
and rejecting rows in partial ones — is worth **−2,038 T, or 0.7%**.

**The arithmetic that kills it.** Skipping 10.9% of columns is worth about
24,100 T/update. Spread over ~29 columns that is a budget of **~800 T per
column** for all coverage bookkeeping. MASKED_E's classifier alone spent
1,975. MASKED_F cut that by deriving the row extent from the two height bytes
and the profile instead of re-deriving it with two signed 16-bit compares and
two `rowfloor` calls, which is worth 12,424 T — real, and still nowhere near
enough. What remains (mask table lookups, the 3-byte owned/range accumulate,
`cov_mark`, and the stash/restore path on partial columns) is ~1,490 T per
column against an ~800 T budget.

**This is not a verdict on coverage, it is a verdict on ORDERING.** The
expensive part of classifying a column is knowing which rows it covers, and
that is exactly what DDA is supposed to make free: carrying an integer row
plus a fractional error removes `row_floor`/`shr3_u` outright, and the row
extent falls out of the walk instead of being recomputed. Coverage on top of
a DDA kernel is a different measurement, and the bench is kept in the repo so
it can be re-run rather than re-argued. **Do DDA first, then re-decide.**

**Design detail worth keeping** (it is why MASKED_E is exact rather than
nearly-exact): the kernel classifies each column once into skip / free /
partial, and a partial column stashes the words an earlier nearer run owns,
draws freely, then puts them back. Gating each store and marking ownership as
it goes would be wrong: within a column the top edge, bottom edge and interior
can share a row (h=0 puts both edges on row 9; LINTEL and RISER move a whole
edge across the horizon), and there the LAST writer must win. Stash-and-
restore keeps last-writer-wins inside the column and first-writer-wins across
runs, which is exactly the host semantics.

**Two bugs the pose oracle caught**, both invisible to any run-scoped test:

1. `row_addr` clobbers DE, and the restore loop held the stashed word there.
   149/311 poses wrong, with name-table entries containing pointer values.
2. Clamping `lo` UP to row 17 when the column starts below the screen. Such a
   column draws nothing, but the clamp made it claim row 17, which then
   suppressed the next farther run's real write there. One wrong cell in
   311 poses — the kind of thing that would have shipped.

**Also measured directly for the first time:** the shipped materializer costs
**221,229 T/update** at pose scope with real map addressing, against the
197,878 previously carried from per-run figures. The pose number is the one to
use — it counts every run of a pose, including the ones the per-run sampling
strided past.

### A25. Coverage / overdraw — image-exact, but the ceiling is 9.1%, not a third.

`make coverage-potential` builds `tools/coverage_potential_probe.c`, which
renders every pose twice from the same geometry (`col_geom()` is shared, so
the two passes differ only in traversal order and masking):

- **far->near, no mask** — exactly what the shipped host path and the current
  Z80 column materializer do. Last writer wins, so nothing can be skipped
  without knowing the future.
- **near->far with a perfect per-cell coverage mask** — the order the shipped
  GG assembly uses. A cell is written once, by the nearest run that covers it.

Measured over **29,824 poses** (yaw step 16, four sub-cell offsets):

| quantity | per update |
| --- | ---: |
| columns materialised, far->near | 29.27 |
| row-writes, far->near | 277.34 |
| row-writes surviving near->far + mask | 252.09 |
| row-writes rejected by the mask | 25.26 |
| columns skipped entirely | 3.16 |

**Work eliminated: 9.1% of row-writes, 10.8% of columns. Images identical,
0/29,824 poses differ.**

**Correction to something I said earlier.** I had implied roughly a third of
materializer work gets overwritten, inferring it from 29.27 columns
materialised against 20 screen columns. That inference was wrong. Runs do
overlap in *columns*, but they largely occupy different *vertical bands*
within those columns, so column-level overlap massively overstates cell-level
overdraw. The row-level number is the real one: 9.1%.

**What this changes.** Near->far + coverage is proven correct and available,
but it buys about one rung of the optimisation ladder (A24's rungs were 2.5%
to 8.4%), not a step change. It is worth building on the Z80 as a normal rung
with an A/B twin, not as an architectural move. The architectural move that
does promise a step change is still *temporal*: stop materializing cells that
did not change between updates. Coverage is spatial dedup within one frame;
that ceiling is now measured at 9.1% and will not grow.

**Bug found while building the probe** (worth recording, it is a trap the Z80
kernel will hit too): the first version masked whole **columns** — if a column
was fully owned, skip it — but the `draw_edge`/`draw_full` calls still wrote
their full row range whenever a column was *not* fully owned, so a far wall
could overwrite a near one. 13,737/29,824 poses mismatched. Masking has to be
per **cell**: draw into a scratch column, then merge only the unowned rows.
The Z80 kernel must do the same — a per-column skip test is not sufficient,
the mask has to gate each row store.

**Unknown, not estimated:** the Z80 cost of maintaining the coverage mask
(18 rows x 20 columns of ownership bits, plus the per-row test in the store
loop) is not yet measured. It could plausibly eat a meaningful fraction of the
9.1%. Nothing is claimed until the A/B twin runs.

### A22. CARRY_EDGE_A — external review's diagnosis was exactly right, and worth 2.5%.

External review identified a precise redundancy in the column materializer:
per column it computes both endpoints from scratch, so the next column's
`invl` recomputes the value the previous column already produced as `invr`.
That reading of the code is **correct** — it is the runtime analogue of what
`SPANC` does in the baker.

`tools/z80_materialize_run_bench.py` (`make materialize-run-bench`) rebuilds
the materializer run-scoped and carries the endpoint. **2,327/2,327 runs
exact** against a per-run oracle dumped by the same self-checking probe.

**The first measurement was a trap, and worth recording as one.** Against the
per-column kernel's 7,798 T/column it looked like a 12.4% *regression*. That
was a confound of my own making: the per-column baseline wrote into a
single-column buffer (`r*2` addressing) while the run kernel writes a real
20x18 map (`r*40 + c*2`). **The 7,798 T baseline was optimistic — it never
paid realistic addressing** — so it is not a fair comparison and is retired
as one.

Re-run as a proper A/B, against a NOCARRY twin identical in every other
respect (same addressing, same helpers, same tile logic):

| | T/column |
| --- | ---: |
| NOCARRY twin | 8,983.5 |
| **CARRY_EDGE_A** | **8,762.5 (−2.5%)** |

**221 T/column — almost exactly the cost of one `shr6_clamp`.** The
redundancy is real, is exactly where review said it was, and removing it buys
2.5%. It is not a step toward the 5.4x the frame budget needs. Recorded
because the diagnosis being right and the remedy being small are different
facts, and conflating them would send the next session down the wrong path.

### A23. Where the materializer's time ACTUALLY goes — profiled, not guessed.

Rather than reason about the next optimisation, the Z80 interpreter was
instrumented to attribute T-states to the enclosing subroutine. 402 runs,
2,749 columns, 23.9M T:

| routine | share | T/column | what it is |
| --- | ---: | ---: | --- |
| `cmps` | **16.3%** | 1,417.5 | signed 16-bit compare, as a `CALL` with push/pop |
| `row_addr` | **14.6%** | 1,270.1 | `r*40` recomputed per row, as a `CALL` |
| `df_loop` | 10.7% | 924.8 | interior fill |
| `de_loop` | 5.6% | 487.3 | edge row walk |
| `shr3_u` | 4.3% | 372.0 | the `>>3` inside `row_floor` |

**Two routines are 31% of the kernel, and neither computes anything about the
picture.** `cmps` is a comparison; `row_addr` is an address. Both are pure
implementation overhead, and both are exactly what review predicted:
"carry the name-table pointer" (+2 / +42 / −38 instead of `r*40`) and
"hoist the invariants out of the row loop".

That is the directed target, and it is now measured rather than intuited.

**Corrected budget** (real map addressing, CARRY_EDGE_A):

| stage | T/update |
| --- | ---: |
| bearing / decode-clip / GATE / column-solve / sort | 47,342 |
| materialize | 256,519 |
| **TOTAL** | **303,861 — 0.20 updates/frame** |

**Methodological note, validated the hard way this run.** Review's advice was
to change one thing at a time and oracle-check each. My first attempt changed
the endpoint carry *and* the addressing model together, and the result read
as a 12.4% regression that would have been easy to misattribute to the carry.
Only the NOCARRY twin — identical but for the one line — recovered the truth.
Every subsequent materializer experiment gets a twin.

### A21. COLUMN MATERIALIZER — built, exact, and it changes the whole budget picture.

`make materialize-bench`. The seam the code review named in its section 12:
the Z80 kernels stopped at the Q6 ramp `(iq, step)` and the emit kernel
started from finished name-table words, with nothing in between. This is that
conversion — `draw_run`'s column body plus `draw_edge` x2 and `draw_full`.
919 bytes. **3,043/3,043 sampled columns exact**, and the full run verifies
15,000 strided across 124,727.

**Oracle** (`tools/materialize_probe.c`) is self-checking in the same way the
column-solve dump was: its column loop must rebuild `tsp_polar_render`'s
*entire* name table for every pose or it aborts. That passed on
**29,824/29,824 poses**, so the dumped rows are the real renderer's behaviour.
The tile emission itself (`draw_edge` / `draw_full` / `edge_entry`) is the
shipped code called directly, not reimplemented.

**Two bugs, both mine, both instructive:**

1. **I used `sbc hl,de`'s carry for SIGNED 16-bit comparisons.** That is an
   unsigned compare: `-7` reads as 65,529, so every edge clamp fired at its
   maximum and 2,751 of 3,043 columns were wrong. A7 had already solved this
   for the decode-clip kernel — XOR bit 15 of both operands — and I simply
   failed to reuse it. Fixed with a `cmps` helper that is that same primitive.
2. **Sign-extending values that are unsigned.** RAISED's
   `bl = 72 + h - (h>>2)` reaches 168, and sign-extending it turned everything
   over 127 negative, blanking every RAISED column. Only `72 - h` can actually
   go negative. Fixed with zero-extending stores where the range says so.

**Measured 7,798 T per column.** Columns materialized per update is **29.27**,
not the 20.00 covered-columns figure — runs overlap, and each overlapping run
materializes the column again (873,084 column-materializations over 29,824
poses). That gives **228,403 T/update**.

**This REPLACES the emit line, it does not add to it** — and that reframes
emit's number. The emit bench measured 21,756 T reconstructing a name table
from finished run words, but those words are pose-dependent, so the runtime
can never be handed them. Producing them is this kernel's job.

| stage | T/update | share |
| --- | ---: | ---: |
| bearing lookup | 5,833 | 2.1% |
| decode-clip | 11,036 | 4.0% |
| GATE | 2,339 | 0.8% |
| column-solve | 26,820 | 9.7% |
| depth sort | 1,314 | 0.5% |
| **materialize** | **228,403** | **82.8%** |
| **TOTAL** | **275,745** | **0.22 updates/frame** |

**UNTUNED, and that matters for how this number should be read.** Every other
kernel here had at least one optimisation pass. This one was written purely
for correctness: every operand goes through memory, the signed compare is a
`CALL`, and `edge_entry` re-reads its inputs per row. It costs **433 T per
name-table word against emit's measured 60.43** — a 7x gap on the same kind of
work, which is a strong hint the gap is implementation and not physics. But
the number is recorded as measured, not as hoped, and nothing should be
planned on the assumption that it will come down until it does.

The honest headline: **the pipeline is now complete and correct end to end,
and it is 4.6x over frame budget**, with 83% of the cost in the one kernel
that has never been optimised.

### A20. Can the draw order be PRECOMPILED instead of sorted? Measured: mostly, but not enough to be worth it.

Three experiments, in sequence. The conclusion is a **do-not-build**, which is
worth as much as a green result and cheaper to act on.

**1. Is recipe insertion order already a valid painter order?** No.
`ORDER_ORACLE_STRICT` in `tools/fused_block_render.c` takes keys straight from
the recipe front end (the block file is not consulted, so it cannot be a
confounder), draws them in exactly that order, and **never calls
`insert_run`**. Result: **30.5526%** of poses exact — bit-identical to
`ORDER_BLOCK`'s 30.5526% and 750,068 mismatched words, which also
cross-validates the block export as faithful to recipe order. Depth ordering
is load-bearing. Only the final 3.2% of the depth-sorted path was a tie
problem.

**2. Pairwise order-flip census** (`make pairwise-order`,
`tools/pairwise_order_probe.c`) — every pair of walls that can co-occur in a
cell, swept over local (x,y) and yaw:

| | pairs | share |
| --- | ---: | ---: |
| order CONSTANT over the whole cell | 10,999 | **72.40%** |
| always EQUAL depth (tie, convention decides) | 284 | 1.87% |
| order FLIPS — a real ownership boundary | 3,910 | **25.74%** |

Of the flipping pairs, **55.45% flip within a fixed yaw** (a genuine
translation boundary) and **44.55% are constant within every yaw slice but
differ between slices** — those have *no* translation boundary at all and an
affine selector in (lx,ly) cannot express them. They are yaw events.

**The intuition about perpendicular distance was right.** Ordering by `invd`
(`wall_d_q4` is affine in camera position, so `invd` is translation-only) is
constant over the cell for **94.08%** of pairs against 74.27% for `inv_mid`.
But substituting it as the sort key does **not** render correctly:
`ORDER_INVD` scores **54.34%**. The yaw-dependent normal-dot and secant terms
in `inv_at_invd` genuinely change which wall owns the screen.

**3. Are the translation boundaries affine?** (`make flip-boundary`) Exhaustive
**64x64** local sweep per pair per yaw, decided by exact convex-hull
disjointness — a decision procedure, not a fitted classifier that could only
ever report success. Ties are excluded from both sides, since equal depth
makes either assignment valid.

- two-sign slices linearly separable: **50.22%** (802 of 1,597)
- flipping pairs where **one axis serves every yaw slice**: **20.11%**
  (37 of the 184 pairs that have more than one slice — and only multi-slice
  pairs actually test the claim, which is why that denominator is stated)

**Verdict.** A fully precompiled ordering would cover the 74.3% that never
flip, plus roughly half of the 25.7% that do, and would still need a fallback.
Call it ~87% at best, bought with selector evaluations at ~860 T each. The
runtime depth sort it would replace costs **1,314 T, or 1.9% of the update**
(A19). **The ownership-selector machinery cannot pay for itself here.** Keep
the sort.

What the census *is* worth keeping: the 72.40% constant-order figure is a
strong hint that a future baker could skip comparisons it knows are decided,
if the sort ever became hot. It is not hot.

### A18. The depth sort is MANDATORY — my own optimism was wrong, and it is cheap anyway

A17 measured baked order at 29.6% and I suggested the fix "might even be
free": emit blocks in the oracle's insertion order and the runtime sort might
become unnecessary. **Tested, and half of that was wrong.**

Switching the baker to recipe order (`ORDER_MODE`, `tools/span_block_bake.py`):

| | bearing order | recipe order |
| --- | ---: | ---: |
| ORDER_DEPTH (with runtime sort) | 96.8% | **100.0%** |
| ORDER_BLOCK (no runtime sort) | 29.6% | **30.6%** |

So the tie-breaking half of the hypothesis was right — recipe order takes the
depth-sorted path from 96.8% to **exactly 100%, 74,560/74,560**. The other
half was wrong: baked order alone moved by one point. **Two different static
orders both land near 30%, which is the practical proof that no static
per-cell order can work** — `inv_mid` depends on sub-cell position *and* yaw,
so the interpreter must sort at runtime. Recorded because I said otherwise in
the previous session and a reader would otherwise carry that forward.

**Cost of switching to recipe order:** SPANC share falls 44.5% -> 29.2%,
payload 31,121 -> 32,068 bytes. **947 bytes, 3.0%.** Cheap for exactness, so
`ORDER_MODE` now defaults to `"recipe"`.

One honest nuance: a tie means two walls at equal midpoint depth, so which
one wins is a *convention*, not a geometric truth. A future baker may pick
either — the requirement is only that baker and runtime agree. Here the
shipped renderer is the definition, so recipe order is what matches it.

### A19. Depth-sort kernel — CLOSED. Cycle-exact, 24,854/24,854, and it is budget noise.

`make depth-sort-bench` (`tools/z80_depth_sort_bench.py`). Since the sort is
now known to be mandatory it became a real budget line, so it was measured
rather than assumed. 62 bytes of Z80.

Verified against the **shipped `insert_run`'s own output** — the fused
renderer dumps, per pose, the `inv_mid` values in insertion order and the
final `g_run_order` the real code produced, and the kernel must reproduce
that array element for element, tie behaviour included. 24,854 poses strided
across all 74,560 (strided, not the file's head — the oracle is written in
map-scan order, so taking the head would have sampled one corner of the map,
the same coverage trap as A16).

**Measured 1,313.6 T/update** (min 135, max 7,932). Workload: mean 4.29 runs
sorted, max 12; mean 5.42 insertion shifts.

| stage | T/update |
| --- | ---: |
| bearing lookup (A12) | 5,833 |
| decode-clip | 11,036 |
| GATE | 2,339 |
| column-solve | 26,820 |
| **depth sort** | **1,314** |
| emit | 21,756 |
| **TOTAL** | **69,098** — 0.86 updates/frame |

The last correctness unknown in the pipeline costs **1.9% of the update**.

### A17. FUSED HOST PATH — BUILT AND EXACT. Baked block in, 20x18 name table out.

`make fused-host-path` (`tools/fused_block_render.c` + `tools/export_blocks.py`).
The end-to-end proof that was missing: a baked block program drives a
complete name table, compared **word for word** against the shipped
renderer's own output over **74,560 real poses** (every walkable cell x 5
sub-cell offsets x every 8th yaw).

**Built so the only difference is the front end.** It `#include`s
`tilesector_polar_renderer.c`, so `project_key`, the bearing field, the clip,
column-solve, `draw_run`, the edge/full materializers and the background fill
are all the *shipped C*. Nothing downstream is re-implemented, so a mismatch
can only come from the block program. A Python re-derivation of the
materializer would have made every mismatch ambiguous.

**Three findings, cleanly separated by rendering each pose three ways:**

| mode | poses exact | reading |
| --- | ---: | --- |
| key SELECTION (as sets) | **74,560 / 74,560 (100%)** | the baker picks exactly the right spans |
| ORDER_BLOCK — strict baked order, no depth sort | 22,039 / 74,560 (29.6%) | **baked order alone is definitively insufficient** |
| ORDER_DEPTH — runtime far→near `inv_mid` sort | 72,179 / 74,560 (96.8%) | depth sort recovers almost all of it |
| ORDER_ORACLE_INSERT — same set, oracle's insertion order | **74,560 / 74,560 (100%)** | **exact** |

1. **Key selection is perfect.** The block bake chooses the right spans at
   every one of 74,560 poses. That half of the baker is proven.
2. **The review's §6 concern is confirmed and quantified.** Drawing in baked
   order gives 29.6% — 851,595 wrong words. Order is not a static property of
   a cell, because `insert_run` sorts by `inv_mid`, which depends on the pose.
3. **The residual is smaller and more specific than "must sort at runtime".**
   Feeding the *same* key set in the oracle's insertion order is exact, while
   the depth sort alone leaves 3.2% wrong. So the gap is purely
   **insertion-order tie-breaking among equal `inv_mid`**: `insert_run` shifts
   only on *strictly* greater, so ties keep insertion order
   (`src/tilesector_polar_renderer.c:534`). The baker must emit a
   tie-consistent order, or the interpreter must break ties identically.
   That is a far smaller problem than a runtime depth sort.

**What this does and does not prove.** It proves the block program is a
faithful front end and locates the exact remaining gap. It does *not* yet
prove a Z80 interpreter can execute it — this is host C, and the Z80 kernels
are still separate. Fusing those is the next step, and this tool is now the
oracle for it.

### A16. Test-coverage failure found by external review — TWO layers, code was innocent

An external review flagged that the column-solve oracle placed the camera at
coarse-cell centres. Checked, and it was right: `px = gx*64 + 32`, and
`32 & 15 == 0`, so **`x_q4 & 15` was zero in all 215,292 rows**. `wall_d_q4`'s
fractional correction (`frac = nx*fx + ny*fy`, then `>>5`) was therefore
identically zero and **never executed once**, in a corpus described as
exhaustively verified.

Fixed by sweeping ten sub-cell offsets, giving 1,071,800 rows. Then a second
layer appeared: a stride-100 sample of the fixed corpus contained **zero
general-path rows** — the non-cardinal segments are only 3 of 17 and uniform
striding simply missed them. So "10,718/10,718 passed" proved nothing about
the code in question, twice, for two unrelated reasons.

Directly targeted, the general path with non-zero fractions passes
4,045/4,045, and the stratified run passes 12,000/12,000. **The code was
correct all along.** The defect was in the verification, not the kernel —
which is worse in one specific way: the claim of verification was not earned,
and nothing would have caught a real bug there.

Durable fix: `stratify()` samples per class (cardinal vs general x
zero-fraction vs non-zero) and **raises on an empty stratum** rather than
quietly reporting a pass. It prints the class census every run. Because a
balanced sample over-represents the general path (50% of sample vs 12.5% of
reality), the T-state figure is now population-weighted: 6,237.2 T/span
weighted against 8,277.3 unweighted. The weighted figure lands within 0.1% of
the previously recorded 6,243.0, so the cost was never wrong — only the
confidence was.

**Generalised lesson, for every oracle in this repo:** a corpus is not
coverage. Every existing bench samples "real poses" the same centres-only
way. `span_block_bake.py` walks sub-cell positions properly, but the bearing,
GATE, decode and emit benches should each be audited for the same class of
hole before their pass rates are cited again.

### A15. Parked efficiency leads and open unknowns

**Standing direction: function over efficiency.** Get a whole update running
end to end on the Z80, then optimise something that demonstrably works.
Everything below is known, unpursued, and deliberately *not* estimated —
this project has now been wrong on five straight estimates (emit 94% low,
decode-clip 190% low, GATE 7.5x low, A11 3.6x high, column-solve 252% low),
and writing another number here would only create something to retract.

**Efficiency leads, unquantified on purpose:**

- **Register-passing for multiply operands.** Every `MA`/`MB` is a store plus
  a load, 5-6 times per span, plus `call`/`ret` per multiply. The single
  biggest structural cost in column-solve. Unknown what it is worth.
- **Re-run GATE on the A12 primitive.** 2,339 T, same two-product shape the
  bearing kernel had; still on the old shift-add loop.
- **emit is the largest single line** at 21,756 T. A1 (LITERAL) and A3
  (retained vs unconditional) apply directly.
- **Bearing cache.** The 9.12-distinct-corners figure is measured, but the
  cache itself is *not built* — the 5,833 T line assumes it exists. That is a
  functional gap, not just an efficiency one; see below.
- **`inv_at_invd` is called twice per span** with the same `sid` and `invd`,
  re-deriving `nx`/`ny` and the cardinal test both times. Hoistable.

**Functional gaps — these matter more than any of the above:**

1. **No end-to-end update exists.** Six verified kernels, each proving its own
   stage against its own oracle, and *nothing that composes them*. Separate
   memory maps, separate harnesses. Until one update runs start to finish —
   block -> decode -> GATE -> bearing -> column-solve -> emit -> name table —
   matching the host oracle's 360 words, the architecture is proven in pieces
   and unproven as a whole. **This is the next thing to build.**
2. **The bearing cache is assumed, not written** (see above). The budget line
   depends on it.
3. **The block walker does not exist.** Something has to read the per-cell
   span program (`span_block_bake.py`'s output), dispatch SPAN/SPANC/GATE/END,
   and feed the other kernels. That is the interpreter's actual main loop and
   no version of it has been written in Z80.
4. **C4: 7 corners with no accurate baked leaf**, `0xff` escape marker
   unhandled at runtime. Still open.
5. **Nothing has run on real hardware or a real emulator.** Every number here
   comes from a from-scratch interpreter. `tools/z80core.py` was validated by
   reproducing A13's independently-built result exactly (44 bytes, 193.0 T),
   which is good evidence and is *not* the same as running on Gearsystem.

**Also unresolved and worth stating plainly:** the five benches predating
`z80core.py` each carry their own assembler/interpreter copy. Consolidating
them onto the shared core is deferred; the check when it happens is that
every bench reproduces its recorded T-state figure exactly. Until then the
duplication is deliberate — swapping the substrate under a published
measurement invalidates it.

### A6. K under cylindrical projection — OPTIONAL COMPARISON, not a gate.

A28 briefly promoted this to "the gate on the temporal architecture". **That
promotion is withdrawn.** A29 gets 51.8% of row-writes off under pure rotation
with the shipped perspective projection untouched, so cylindrical is not a
prerequisite for anything.

What remains true and worth keeping: under a cylindrical mapping the image at
yaw psi is `F(psi + k*(x-80))` for a static scene, so a yaw step is an exact
rigid shift the scroll register absorbs for free, whereas under the shipped
tangent mapping a 12-yaw-unit update shifts the screen centre 24 px and the
edge 37 px and no rigid shift exists. That makes cylindrical a useful **upper
bound experiment** — what the temporal renderer could reach if yaw were free —
and nothing more. **The visual target is the existing perspective viewport.**
Do not change the projection to rescue a metric.


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
