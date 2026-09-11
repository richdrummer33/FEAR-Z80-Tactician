# Handoff: temporal span-delta materialization

Branch: `claude/sms-architecture-diagram-law8ti`
Repo: `richdrummer33/FEAR-Z80-Tactician`
Last commit at handoff: `86e8a8b` "DDA_G: -12.0% by walking row extents instead of dividing for them"

## What this project is

Sega Game Gear (Z80 @ 3.579545 MHz) 2.5D renderer. **Not real-time rendering** —
a PC baker (`tools/`, host C) compiles a lightweight structural representation
of visibility into ROM. The Z80 is a span interpreter: it evaluates cheap
local equations and streams tile words into a 20x18 name table (160x144 LCD,
8px tiles). Frame budget is 59,736 T at 59.9 Hz; the update currently costs
242,103 T, i.e. **0.25 updates/frame** — well under real-time, which is fine
because the design goal was never real-time.

## Standing rules (still in force)

1. State what step(s) you're doing, end with what's done and what's next.
2. Don't over-invest in getting T-estimates exact — log unknowns in
   `docs/TODO_DEFERRED.md` rather than polishing a guess. Function over
   efficiency was the early-session priority; the project has since moved
   into a measured optimization phase (see below) but the "log unknowns,
   don't guess" habit still applies.
3. **Verification discipline**: everything is checked against a C oracle
   (usually `#include`ing the shipped renderer source directly, so statics
   are reachable without modifying it), self-checking, and exact — not
   "close enough". A/B twins differ in exactly ONE change so a regression
   or win can be attributed. Re-profile after every change; do not assume
   the cost ranking holds — it has been wrong before (A27 flattened the
   profile completely).
4. `docs/TODO_DEFERRED.md` is the durable record: "If it is only in a CI log
   or a chat reply, it does not exist. Numbers live here." Nothing is
   removed until done or explicitly abandoned with a reason.

## Where things stand (budget table, current)

| stage | T/update |
| --- | ---: |
| bearing lookup | 5,833 |
| decode-clip | 11,036 |
| GATE | 2,339 |
| column-solve | 26,820 |
| depth sort | 1,314 |
| materialize (DDA_G) | 194,761 |
| **whole update** | **242,103** |
| **updates/frame** | **0.25** |

Materialize is still ~80% of the update and is the only remaining large
target. Full ladder that got it there: `docs/TODO_DEFERRED.md` sections
A20–A27 (§A. Z80 span-interpreter micro-architecture).

## What was just closed out (this session)

- **A25**: measured overdraw directly (host C, `tools/coverage_potential_probe.c`).
  Corrected an earlier over-estimate ("roughly a third" was wrong) — actual
  ceiling is **9.1% of row-writes**, because runs overlap in columns far more
  than they overlap in rows.
- **A26**: built the near→far + coverage-mask Z80 kernel
  (`tools/z80_materialize_masked_bench.py`, `tools/materialize_coverage.asm`,
  `tools/materialize_coverage_fast.asm`). Exact at **pose scope** (a new
  oracle format — coverage state crosses runs, so a per-run oracle can't see
  it; see `coverage_pose_oracle.txt` dump in `coverage_potential_probe.c`).
  **It loses**: +19.7% to +25.3% over the unmasked kernel. The classifier
  alone costs far more than coverage could ever save (58,073 T cost vs
  ~24,100 T of potential column-skip savings).
- **A27**: built DDA (`tools/materialize_dda.asm`,
  `tools/z80_materialize_dda_bench.py`). Row extents come from one byte
  compare per column instead of six row_floor/signed-compare derivations;
  sub-row offset is carried by −8 per row instead of rebuilt. **−12.0%**,
  pose-exact on 311/311. Re-ran coverage ON TOP of DDA (MASKED_H) to check
  whether cheap row-extents changes the coverage verdict — it doesn't:
  still +7.7% over DDA_G alone. **Coverage at column granularity is closed**,
  not deferred — measured twice, on two kernels, losing both times.

## WHERE THIS ACTUALLY LANDED — read A29 in `docs/TODO_DEFERRED.md`

Two probes, in this order, and the second corrects the first.

**A28** (`make temporal-delta`) asked whether a whole `(run, column)` work key
survives an update. Under rotation: 0.1%. It concluded the direction was gated
on cylindrical projection. **That conclusion was withdrawn.** A whole-key match
is not the condition for skipping work, and the tell was inside A28's own
numbers: 70.8% of cells unchanged under the same rotation that gave 0.1% key
stability.

**A29** (`make temporal-boundary`) asks the right question. A wall column is a
top edge, a bottom edge, and an interior of identical FULL tiles. The retained
state is `(tl, tr, bl, br, shade, border)` per column, which is provably the
complete determinant of that column's output. Interior rows that stay interior
at the same shade and border are not dirty however far the geometry moved.

**With the perspective projection untouched**, on A28's corpus:

| | all regimes | pure rotation |
| --- | ---: | ---: |
| row-writes now | 230.54 | 275.61 |
| dirty after the boundary filter | **23.8%** | **48.2%** |
| exact floor | 19.4% | 38.1% |
| columns fully skippable | 41.7% | 1.2% |
| columns needing only boundary work | 35.8% | **49.9%** |

Both checks clean on all 879,808 pairs: state-only re-derivation equals the
renderer's output, and no cell outside the dirty set ever changed.

**Cylindrical projection is NOT a gate.** A6 says so now. It stays available as
an optional upper-bound comparison and nothing more. Do not change the
projection to rescue a metric.

**A30** (`make temporal-cadence`) then swept the update period, because U=4 is
the OLD renderer's cadence and designing against it bakes in its speed. The
workload is a feedback loop and it moves a lot:

| pure rotation | U=4 (dyaw 12) | U=1 (dyaw 3) |
| --- | ---: | ---: |
| dirty | 48.2% | **27.2%** |
| columns edge-only | 49.9% | **61.1%** |
| V_COLUMN_SHIFT | 45.0% | **16.0%** |
| spans moving 0 or 1 column | 27% | **97.3%** |
| `tile_id += delta` reachable | 30.1% | **53.7%** |

So V_COLUMN_SHIFT is a `SHIFT_RUN dx=+/-1` case at realistic cadence, not an
N-column rebuild. Restoration is measured too: 21.9% background, 74.0% a span
already in retained state, 4.1% new. The underlay cache and the next-owner
pointer are both refuted (5.1% and 27.5%); what works is a near->far scan over
retained state, mean depth ~3.

**Two things A30 says NOT to assume.** One shared delta per span does not
improve with cadence (16-19% under rotation at every U), so most edge-only
columns still need per-column geometry when turning. And the tail is topology,
not motion rate: across all regimes the max dirty stays pinned at 360 at every
cadence while the mean halves, because doorway crossings do not get cheaper
when you render faster.

**A31** built the A/B foundation and found the trap in it. `make
temporal-bench` emits a pose-SEQUENCE oracle, since a temporal kernel carries
state across poses and every earlier bench verifies one pose. Unmodified DDA_G
reproduces all 300 poses exactly at 153,450 T/update over 26.57 columns, so the
oracle is proved before anything is built on it.

**The obvious first rung is incorrect.** A purely local "if this span's column
state is unchanged, skip the column" — no cross-span union, no second pass — is
wrong on 76.7% of pose pairs under rotation at U=1, and on 30.9% across all
regimes. In far->near order an unchanged span does write the same words, but a
nearer span may have moved away and uncovered cells it owns, leaving stale
pixels. Under 5% of cells wrong on three quarters of frames, so it would look
almost right.

So `TEMP_BOUNDARY_A` must build the cross-span dirty union before drawing,
which is the shape whose cost killed A26. **The classifier cost is the whole
question.**

**A32** (`make temporal-error`) tested bounded-error rendering, H-scroll and a
semantic metadata layer. All three are closed; one surprise came out of it.

Bounded error buys 12.5% off the exact path at 1 px under rotation at U=1, and
15.7% across all regimes, in exchange for 7-9 visibly wrong cells per frame
with peaks near 90. The bound holds exactly and convergence after motion stops
is one frame, but the saving is too small for the visual cost, and it shrinks
with cadence. Not because deferral wastes itself on free transitions - 87.8% of
deferred columns would have changed a tile - but because the dirty set is
dominated by column shifts and topology, which are never deferrable.

A per-pixel H-scroll leaves 73.1% of endpoints within 1 px under rotation,
much better than A28's whole-column 0.6%, but it cannot touch heights and only
64.5% of columns are within 1 px vertically, so under half qualify on both
axes.

The three unused name-table bits lose on maintenance: 9.27 cells change
semantic class per update against about 1.8 restoration scans saved.

**The surprise, and it is good news.** A31 called the mandatory cross-span
union "the whole question" because A26 died on classifier cost. Counted, the
union is 22.30 six-byte state comparisons and 33.07 row-range marks per update.
It is not A26's shape: A26 was expensive because knowing a column's row extent
was expensive, and here the extent comes from retained state for free.

**A33** (`make union-bench`) built the cross-span union as real Z80 and
measured it. **This is the go/no-go and the answer is negative in this form.**

| U=1 corpus | union alone | % of DDA_G's 153,450 T | p95 as % |
| --- | ---: | ---: | ---: |
| pure rotation | 84,307 T | 54.9% | **132%** |
| all regimes | 53,282 T | 34.7% | **123%** |

The tail disqualifies it, not the mean: on 5% of updates the union alone costs
more than a complete render, before a cell is drawn or restored.

Four rungs, one mechanism each, all verified as a superset of the host's mask
except the last. UNION_A's profile-free conservative range marks 249.81 cells
against the host's 71.65 and throws away the interior-resident mechanism.
UNION_B fixes that and costs 5.7% MORE. UNION_C's table-driven marking returned
3.8% against a profile that attributed 33% to marking, because there are only
~40 marks per update. UNION_D, the restructure that removes the per-column
presence test, is NOT CORRECT yet and its number is not a result.

Re-profiled: the cost is per-column iteration and presence testing, not row
extents. A32 was right that the extents are free; the cost simply moved.

**A34** (`make span-stream`, `make union-stream`) tested whether the span, not
the column, is the right retained unit. Answer: it is the right thing to STORE
and COMPARE, and the wrong thing to WORK FROM.

The 6-byte record (sid, inv0, inv1, c0, c1, flags) reconstructs the name table
**pixel-for-pixel on all 1,789,440 frames**. 3.19 visible spans per frame, 19.2
retained bytes against the column form's 71.7, a 3.74x reduction. And
`draw_run` reads only `x0>>3` and `x1>>3`, so an edge is 5 bits, not a byte -
sub-tile X is not part of the exact state at all.

But replacing the columns with it is **1.24x to 1.55x SLOWER**, because the
heights then have to be recomputed from the iq/step walk instead of read. The
comparison saving is ~600 T; the recomputation costs thousands. Per-column
derivation is the expensive axis and the span form adds to it.

Used as a PRE-CHECK beside the column form it does pay: UNION_E is **-14.7% on
mixed motion** and +1.6% under pure rotation, where nearly every span changes
and the check never fires.

Also: `inv_for_dq4` is a pure function of |dq4| and a **2,033-byte exact table**
removes its interpolation with no banking. The bigger `inv_at_invd` tables are
27-47 KB for a prize of about 1.7% of the update, so they are not worth
banking for.

**A35** fixed the p95, and it turned out to be my own conservatism rather than
topology being expensive. When two retained spans swap draw order only the cells
they BOTH cover can change winner; A33 marked every span in both streams whole.
That one rule was the entire tail: inversions fire on 11.2% of rotating updates
and cost 202,883 T against 73,189 T without.

| corpus | best variant | T/update | % of a render | p95 % |
| --- | --- | ---: | ---: | ---: |
| U=1 rotation | UNION_C | 76,019 | 49.5% | 91% |
| U=1 all regimes | **UNION_E** | **40,503** | **26.4%** | 85% |

Against A33's 34.7-54.9% with a p95 of 123-132%. **The union's worst case is now
below a full render rather than above it.** A34's span-record pre-check confirms
at -16.8% on mixed motion.

UNION_G, hoisting the marking setup out of the per-column loop, LOSES: the prep
call and its register preservation cost more than the two table lookups it
saves. Fourth time a profile share has failed to convert.

**NOW build TEMP_BOUNDARY_A**, on UNION_E, against the verified sequence
oracle. The union is finally cheap enough and bounded enough that the
executor's cost is what decides the architecture. UNION_D is still incorrect
and its target, the per-column presence test, is still the largest coherent
item at 17.9%.

**Superseded below.**

**Do NOT build TEMP_BOUNDARY_A yet.** It would sit on a union costing half a
render. Finish UNION_D first (three sweeps: overlap, new-only, old-only, no
inner-loop presence test), then answer the p95 separately - a full render is
bounded at 153,450 T and the union's p95 is not, so the exceptional path needs
a budget cap or a fall-back-to-full-render rule.

**Retained state is (hl, hr, border), never (il, ir):** every endpoint derives
from il>>1, so retaining il reports change where there is none on 4.9% of
columns. Asserted on the host now, 0 disagreements.

**Superseded plan below; read A33 first.** Build `TEMP_BOUNDARY_A` against the
verified sequence oracle, costed
against the U=1/U=2 distributions rather than U=4, with the interior-resident
path as the valuable half (full skipping is 13.1% of columns under rotation,
edge-only is 61.1%). Then B run-shift, C tile-delta, D grow/shrink, E
restoration, one rung at a time and re-profiled after each.

Also priced along the way: `polar_transition_bake.c` emits **136.58 MiB**
against a 128 KiB target, so pose enumeration is out.

## The open question as it was originally posed (kept for context; answered above)

> "temporal question of not materializing unchanged cells at all"

This is a DIFFERENT mechanism from A25/A26's spatial coverage (dedup within
one frame). The idea: across successive updates (player moves a little,
camera rotates a little), most of the screen doesn't change. If the baker or
runtime can identify which spans/cells are unchanged from the previous
update, the materializer could skip re-deriving and re-writing them
entirely — this is a much larger ceiling than the 9.1% spatial number,
potentially the actual step-change the project has been looking for since
A24.

**Nothing has been designed or built yet for this.** The user has been doing
independent brainstorming (with ChatGPT) on a fresh angle for this — expect
a pasted excerpt with specifics in the next message of the new chat. Read
it as one perspective, not gospel, per the standing review discipline
established earlier this project (external review handling: confirm what's
actually right, push back on what's wrong or already covered, don't
rubber-stamp).

## Key files to re-orient with

- `docs/TODO_DEFERRED.md` — read section A (all of it, especially A20–A27)
  for the full derivation history and every measured number.
- `tools/materialize_dda.asm`, `tools/z80_materialize_dda_bench.py` —
  current fastest materializer kernel (DDA_G), the base to build temporal
  logic on top of.
- `tools/coverage_potential_probe.c` — host-side measurement harness
  pattern (col_geom shared between passes so passes differ only in the
  thing being tested); same pattern will likely be needed for a temporal
  probe (host C measuring frame-to-frame span/cell deltas before any Z80
  kernel is built — "measure before building" has paid off twice now).
  Also the source of `coverage_pose_oracle.txt`, the pose-scope oracle format.
- `tools/z80core.py` — shared Z80 assembler/interpreter (regular-encoding
  derived, not hand-listed opcodes). Used by all `z80_*_bench.py` tools.
  Has a bisect-based per-label profiler pattern now (see
  `z80_materialize_dda_bench.py`'s `profile()` function) — reusable for
  profiling whatever temporal kernel gets built.
- `Makefile` — targets: `coverage-potential`, `masked-bench`, `dda-bench`,
  `materialize-run-bench`, `materialize-bench`, `fused-host-path`,
  `depth-sort-bench`, `pairwise-order`, `flip-boundary`, plus the earlier
  bearing/column-solve/qsquare benches.

## Answering the user's question about cross-chat references

Claude Code sessions are isolated by default — a new chat has no access to
this session's conversation history or working memory. What DOES carry over
automatically:
- Anything committed to the git repo (code, `docs/TODO_DEFERRED.md`, this
  handoff file) — a new session starts by reading the repo fresh.
- This file, if committed, IS the bridge — paste it or point a new session
  at `docs/HANDOFF_TEMPORAL.md` and it has full re-orientation.

What does NOT carry over: uncommitted reasoning, anything only said in chat,
build/ directory artifacts (gitignored on purpose).

There is a `SendMessage`/`ListAgents` mechanism for one live session to
message another live session or subagent it spawned, but that requires both
sessions to be running concurrently and addressable — it's not how you'd
hand off to a *new* chat you're about to start. For a fresh chat, committing
this file and pointing the new session at it is the right mechanism.
