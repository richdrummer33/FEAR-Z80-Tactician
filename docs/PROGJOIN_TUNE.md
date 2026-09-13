# PROGJOIN dispatch tuning — selected-count body header

Date: 2026-09-13
Branch: `claude/renderer-forensic-reconstruction-azzocg`
Baseline ground truth: `b9f76ac`
A/B harness: `tools/z80_progjoin_tune_bench.py`
CI: `.github/workflows/progjoin-tune.yml`

## Question

Can the joined compiled-edge-program path remove per-chunk pointer chasing by
using the fact that `want` (the number of columns played by this chunk) is
already part of the descriptor key?

The baseline body format is:

```
[C+1 cumulative prefix-count bytes][4-byte cell records...]
```

After dispatch selects a body, the kernel still reloads `want`, indexes the
prefix header, saves the body pointer, reloads it, skips the header, and only
then points SP at the playback stream.

Because `want` is already encoded in the descriptor dimension, that work is
redundant. The tuned representation used by this experiment is:

```
[1 selected cell-count byte][4-byte cell records...]
```

The dispatch block remains a 16-bit body offset. This specifically avoids the
rejected four-byte-entry design, which would approximately double dispatch ROM
and threaten the 4 MiB cartridge ceiling.

## Experimental method

The committed baker and baseline kernel are left unchanged so the control is
not contaminated by the optimization. For every emitted window, the A/B harness
builds a second equivalent table set from the original bytes:

1. Read the exact same `progjoin_cases.txt` workload.
2. Determine each active `(slot, family, want)` descriptor.
3. Read the old body's selected prefix count at `body[want]`.
4. Copy exactly that many existing 4-byte cell records into a tuned body,
   prefixed by one count byte.
5. Rewrite only the corresponding 16-bit dispatch-block body pointer.
6. Execute the original and tuned Z80 kernels over the same corpus cases.
7. Compare both outputs against the renderer oracle using the existing
   PROGJOIN correctness checks.

This makes the comparison apples-to-apples and keeps rollback trivial.

## Full-corpus result

GitHub Actions run `34790945190` completed successfully.

| metric | baseline | tuned |
| --- | ---: | ---: |
| covered run-edges | 19,912 | 19,912 |
| dispatches | 31,806 | 31,806 |
| cells played | 160,717 | 160,717 |
| wrong cells | 0 | 0 |
| stray writes | 0 | 0 |
| faulted run-edges | 0 | 0 |
| oracle verdict | EXACT | EXACT |
| joined-path cycles | 55,288,303 T | 52,266,733 T |
| per run-edge | 2,776.6 T | 2,624.9 T |
| dispatch per chunk | 1,041.7 T | 946.7 T |
| playback per cell | 68.2 T | 68.2 T |
| chunk advance | 245.1 T | 245.1 T |
| kernel size | 293 B | 274 B |
| largest-window body blob | 6,003 B | 4,677 B |

**Saved: 3,021,570 T over the full corpus, or 5.47% of the joined edge path.**
The saving is exactly where intended: dispatch falls by **95.0 T per chunk**;
playback and chunk advance are unchanged.

The largest-window body blob also shrinks by 1,326 bytes (22.1%) in this A/B,
because six prefix bytes disappear from each selected program and the resulting
records re-deduplicate. The global multi-megabyte corpus size must be re-baked
before quoting a global ROM saving; this windowed result is not that census.

## Whole-update implication

The GUARDBAND composition before this rung was:

- materializer: 83,627 T/update
- whole update: 129,673 T/update
- implied rate: 27.6 updates/s

This optimization saves about **1,215 T per corpus pose**
(`3,021,570 / 2,486`). Holding all other composed terms constant gives an
updated first-order estimate of approximately:

- materializer: **82,412 T/update**
- whole update: **128,458 T/update**
- implied rate at 3,579,545 Hz: **27.87 updates/s**

These are **COMPOSED**, not target-measured FPS. The 20 Hz solid target remains
comfortably cleared. The 30 Hz aspiration at 119,318 T/update is still about
9,140 T away, so this optimization is a real win but not the missing 30 Hz
breakthrough by itself.

## Verdict

**KEEP.** It is oracle-EXACT, strictly faster, smaller in kernel bytes, and does
not enlarge the 2-byte dispatch entries.

The important negative finding is equally useful: body-header pointer chasing
was only ~95 T of the ~1,042 T dispatch. The remaining ~947 T/chunk is now the
real target. Further tuning should profile the remaining dispatch into named
sub-stages before changing representation again: step-to-slot lookup, threshold
rank scan, base extraction, descriptor/block address formation, and final
block/body pointer lookup. Do not assume another hoist will dominate; run-edges
average only ~1.60 chunks, so per-edge hoists have limited leverage.

A secondary target is the measured 245.1 T/chunk advance path, especially the
`djnz` repeated-add used for `iq += want*step`. It should be compared against the
remaining dispatch components only after those components are individually
priced.
