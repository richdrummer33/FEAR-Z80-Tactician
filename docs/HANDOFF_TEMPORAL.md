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

**Next, in order:** `V_COLUMN_SHIFT` as a run operation (45.0% of rotation
events); then the vacated-cell restoration mechanism, which is the real blocker
and has no cheap candidate; then and only then a Z80 executor costed from this
project's measured kernel costs and proved with an A/B twin against DDA_G.

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
