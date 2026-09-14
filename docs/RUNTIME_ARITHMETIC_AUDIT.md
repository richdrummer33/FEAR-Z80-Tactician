# Runtime arithmetic / dispatcher audit — 2026-09-13

Branch: `claude/renderer-forensic-reconstruction-azzocg`

This rung followed the selected-count PROGJOIN optimization and asked where the
remaining runtime-derived arithmetic actually goes. Every kept result below was
executed as Z80 machine code in `tools/z80core.py` and checked against the
existing renderer oracle. Whole-update implications remain COMPOSED, not target
measured.

## 1. PROGJOIN dispatch autopsy

Harness: `tools/z80_progjoin_dispatch_audit.py`
CI run: `34793297043`

Full corpus: 19,912 run-edges, 31,806 chunks/dispatches, 160,717 cells; both
control and finite-advance variants were oracle-EXACT.

| tuned dispatch sub-stage | T/chunk | share |
| --- | ---: | ---: |
| want/min | 45.7 | 4.8% |
| step -> slot | 61.0 | 6.4% |
| phase/u | 77.0 | 8.1% |
| threshold address | 78.0 | 8.2% |
| rank scan | 156.0 | 16.5% |
| base extraction | 62.0 | 6.5% |
| descriptor formation | 242.0 | 25.6% |
| block entry | 138.0 | 14.6% |
| body pointer | 41.0 | 4.3% |
| count/join | 46.0 | 4.9% |
| **total** | **946.7** | **100%** |

This establishes that descriptor + block-entry work, not body playback, was the
largest remaining dispatcher component.

### Finite C=6 chunk advance

Replacing the `djnz` repeated-add implementation of `iq += want*step` with six
specialized paths was exact but small:

- chunk advance: 245.1 -> 237.0 T/chunk
- kernel: 274 -> 321 B
- corpus saving: 256,890 T = 0.49% of joined edge path
- about 103 T/update over 2,486 corpus poses

KEEP as a valid exact micro-optimization, but it is not a major lever.

## 2. DPSOLVE finite small-multiplier experiment

Harness: `tools/z80_dpsolve_finite_audit.py`; corrected runner
`tools/z80_dpsolve_finite_audit_v2.py`.
CI run: `34793602325`.

The first run correctly failed the oracle because the experiment's jump-table
lookup clobbered HL/DE before an unrolled case. The corrected version reloads
`iq` and `step` in each case. Full corrected corpus: 126,809 successful
screen-depth-plane rows, zero mismatches in all variants.

| variant | T/span | code | delta |
| --- | ---: | ---: | ---: |
| baseline | 4,945.6 | 1,345 B | — |
| finite `n*step` only | 4,967.6 | 1,764 B | +22.0 T |
| finite c0 walk + finite `n*step` | 4,923.6 | 2,153 B | -22.0 T |

At 4.30 visible spans/update the fastest variant saves only about 95 T/update
while adding 808 bytes. Therefore the general lesson is negative: these small
DPSOLVE loops are not worth a large kernelization effort in their present form.

The census also measured 12.95 depth-plane 16-bit add iterations per visible
run (13.06 per successful solve), 1,919 distinct step values, and all 1,024
phase values reached.

## 3. Run-edge invariant hoist — KEEP

Harness: `tools/z80_progjoin_edge_hoist_audit.py`
CI run: `34793865697`.

Observation: family and step are constant for an entire run-edge, therefore so
are:

- step -> slot,
- `THRESH + slot*8`,
- `DESC + slot*64 + family*C*2`.

The previous tuned dispatcher recomputed all of these per chunk. The hoisted
kernel resolves them once per edge and stores threshold/descriptor base
pointers. It does not modify any emitted table bytes.

Full corpus:

- 19,912 run-edges
- 31,806 chunks = 1.597 chunks/edge
- BOTH EXACT
- tuned control: 52,266,733 T
- edge-hoisted: 50,479,935 T
- saved: **1,786,798 T = 3.42% of joined edge path**
- per run-edge: 2,624.9 -> 2,535.2 T
- per-edge setup: 167.0 -> 475.0 T
- dispatch: **946.7 -> 697.7 T/chunk**
- playback: unchanged 68.2 T/cell
- chunk advance: unchanged 245.1 T/chunk
- kernel: 274 -> 285 B
- emitted tables: byte-identical

Over the 2,486 actual corpus poses, this is about **719 T/update saved**. Holding
all other composed terms constant, the previous 128,458 T pure-yaw whole-update
estimate becomes approximately **127,739 T/update**, or about **28.0 implied
updates/s** at 3,579,545 Hz. This is COMPOSED, not target-measured FPS.

## 4. Post-hoist dispatcher profile

Harness: `tools/z80_progjoin_posthoist_profile.py`
CI run: `34793948416`.

Oracle faults: zero. PC-attributed sub-stages cross-check exactly against the
697.7 T/chunk broad dispatch region.

| post-hoist sub-stage | T/chunk | share |
| --- | ---: | ---: |
| want/min | 45.7 | 6.6% |
| phase/u | 77.0 | 11.0% |
| threshold pointer load | 16.0 | 2.3% |
| **rank scan** | **156.0** | **22.4%** |
| base extract | 62.0 | 8.9% |
| descriptor want-offset + block-base lookup | 116.0 | 16.6% |
| **block entry indexing** | **138.0** | **19.8%** |
| body pointer | 41.0 | 5.9% |
| count/join | 46.0 | 6.6% |
| **total** | **697.7** | **100%** |

### Next rung

The next largest individually isolated cost is rank scan at 156 T/chunk,
followed closely by block-entry indexing at 138 T/chunk and the reduced
descriptor stage at 116 T/chunk. The rational next experiment is a ROM-size
census plus exact A/B for replacing threshold comparisons with a direct
slot-specific `u -> rank` table. Because slot is now hoisted per edge, such a
rank table could also have its base pointer hoisted. Do not adopt it until its
whole-corpus ROM cost, banking implications and exact cycle saving are measured.
