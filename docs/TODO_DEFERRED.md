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
