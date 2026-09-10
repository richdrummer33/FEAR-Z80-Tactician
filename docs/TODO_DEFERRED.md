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

**Verdict.** Temporal skipping is real and large for translation (60–75%) and
worth essentially nothing under rotation (0.1%). Since a first-person camera
rotates constantly, the naive temporal skip is not the step change this project
has been looking for. It is not closed the way coverage is closed — it is
gated, on one specific thing:

> **A6 (cylindrical projection) is a PREREQUISITE for the temporal
> architecture, not an independent optimisation.** Under a cylindrical mapping
> the image at yaw psi is `F(psi + k*(x-80))` for a static scene, so a yaw step
> is an EXACT rigid shift, absorbed by the scroll register for free, leaving
> only newly exposed columns to materialize. Under the shipped tangent mapping
> it is a homography and nothing rigid survives.

A6 is currently parked as a minor question about where K comes from. It should
be re-read as the gate on the whole temporal direction.

**What would close A28:** measure the same delta under a cylindrical
`angle_x`/`sec` pair. That is a host-side change to two baked tables plus a
re-run of this probe — no Z80 work — and it decides whether the temporal
architecture is available at all. Do NOT build a Z80 temporal kernel first.
That ordering is what A25 and A26 established, twice.

**Unknowns, logged rather than guessed** (this project has now been wrong on
five straight estimates, all in the same direction):
- The Z80 cost of any certificate/skip test. Not estimated. A26's classifier
  came in at 1,975 T per column against an ~800 T budget; there is no reason to
  assume a temporal predicate is cheaper until one is built and profiled.
- Whether a cylindrical projection is visually acceptable, and what it costs
  elsewhere in the pipeline. A6 notes wall tops become cosine arcs needing a
  second-order walker.
- Whether the 77.5% store-level redundancy can be reached by any predicate
  cheaper than the derivation it would skip. Currently no candidate.

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

### A6. K under cylindrical projection — PROMOTED: this is the gate on A28.

**Read A28 first.** This entry was written as a minor question about where the
per-span constant K comes from. A28 measured the temporal delta and found that
the whole temporal architecture depends on this choice: under the shipped
tangent mapping a yaw step is a homography and nothing rigid survives (best
whole-column shift compensation buys 0.6%), while under a cylindrical mapping
the image at yaw psi is `F(psi + k*(x-80))` for a static scene, so a yaw step
is an exact rigid shift the scroll register absorbs for free. It is no longer
a projection-flavour preference; it is the prerequisite.


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
