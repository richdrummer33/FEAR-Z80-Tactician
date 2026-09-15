# Finite-program ROM parity experiment scope

This is the next architectural parity experiment after the current DDA/hoist rungs are exact-ROM green.

## Important correction from the executable target audit

The accepted Z80 target is not a small replacement for `draw_edge_row$`. The measured PROGJOIN path compiles and executes an entire **run-edge** as chunks of up to six columns. A generated program body contains the cell sequence for those columns and the signed name-table destination deltas between cells/columns. Dispatch selects a program from run-edge family, step, phase/base and threshold rank. The body is then played directly into the name-table buffer.

That distinction matters. Replacing only the existing GG edge-LUT lookup with a finite tile lookup would reproduce only a representation detail while retaining the old per-column skeleton. It would not be architectural parity.

The Z80 target currently proves, over 2,520 corpus poses, 19,912 run-edges, 31,806 chunks and 160,717 played cells, that this run-edge program model is exact. The selected-count tuning reduces the measured edge-path total to 52,266,733 T over the corpus; hoisting run-edge threshold/descriptor bases reduces it further to 50,479,935 T. The post-hoist dispatcher is still 697.7 T/chunk, so dispatch remains a major cost rather than something to hide inside a composed estimate.

## Whole-corpus cartridge census

The research harness bakes bounded windows because its executable test uses a flat 64 KiB address space. A cartridge cannot inherit that assumption. Unioning all 2,486 oracle poses across 63 windows produced, for FULL top/bottom edges alone:

- 8,919 observed FULL run-edges;
- 16,056 chunks;
- 549 distinct steps;
- 1,732 observed `(step,family,want)` descriptors;
- 4,311 observed `(step,family,want,base,rank)` semantic choices;
- 2,816 unique raw serialized bodies;
- zero cross-window semantic conflicts.

Keeping the research target's dense dispatch lattice would cost about 355,220 bytes for this FULL subset. Most of that is sparse block structure, not useful playback data. The exact sparse-direct cartridge representation in `tools/gg_progjoin_sparse_direct.py` reduces the same observed semantics to 113,766 bytes with byte-exact round-trip of every one of the 4,311 choices. Selected-count program bodies themselves occupy 73,094 bytes across five 16 KiB body banks, with only 90 bytes of bank-boundary padding.

Therefore the dense research tables are a proof format, not the shipping cartridge format. The semantic contract is the selected program; the GG representation is free to remove empty lattice entries so long as every observed choice round-trips exactly.

## First ROM scope

Restrict the first executor to geometry-only FULL walls, but make the unit of work a **FULL run-edge**, not an individual edge row.

Preserve the Game Gear-specific semantics that surround the executor:

- near-to-far persistent ownership / coverage;
- name-table storage and logical visible 20x18 result;
- dirty tracking;
- final tile-word emission and shade/material selection;
- the existing slow edge path as fallback for unsupported/saturated cases.

Inside that envelope, the candidate must:

1. derive or consume the same run-edge key dimensions as the Z80 target;
2. split the edge into the same finite chunks, initially maximum C=6;
3. select a generated body from the same finite vocabulary;
4. execute body cells and signed destination deltas directly, rather than re-entering the old per-column edge-row derivation;
5. retain the existing GG path for inverse-depth-saturated or absent sparse-selector cases.

The first semantic ROM probe deliberately uses a 512 KiB experimental cartridge and fixed Frame-2 asset banks. That separates the architectural question - can real GG banking, dispatch and playback reproduce the target? - from the later shipping-size question. Do not treat the enlarged probe ROM as a proposed final cartridge layout.

## Live ownership without a guard name table

The Z80 research target uses a 20x32 guarded name-table buffer, seven scratch rows above and below the visible 20x18 viewport. That costs +560 bytes of WRAM and makes clipped trajectories branch-free.

The playable GG renderer already has a compact 3-byte-per-column ownership map, so the live executor does not need to duplicate the research WRAM trade. A full-corpus census reconstructing every selected FULL cell found:

- 56,296 visible cells and 17,887 offscreen guard cells;
- zero odd/damaged name-table destinations;
- coverage-byte cursor delta is **0..3 for every consecutive cell**, including while carrying the coordinate arithmetically through offscreen guard rows;
- first-cell coverage seed range is only **-1..60**.

That permits one byte of live metadata per compiled cell plus one signed-byte seed per run-edge. Proposed cell tag:

- bit 7: visible / ownership-test required;
- bits 6..4: row bit within the coverage byte;
- bits 1..0: coverage-byte cursor delta, 0..3;
- bits 3..2: currently spare.

Offscreen cells advance the coverage cursor but never dereference coverage RAM. This preserves the branch-light continuous trajectory without allocating the +560-byte guard name table. The exact tag representation still needs a ROM-size A/B because one added byte per selected cell enlarges program bodies, but the representability question is settled over the full observed corpus.

## Live integration seam

The compiled edge pass must see **pre-surface ownership**, because the current edge-row path tests the unclaimed mask before the surface claims its span. The safe first integration ordering is therefore:

1. At run scope, resolve and execute the compiled FULL top/bottom edge programs against the ownership state left by nearer surfaces.
2. Emit only visible cells whose ownership bit is still unclaimed; offscreen cells only advance compiled cursors.
3. Run the existing column materializer to mark the surface ownership span and fill interiors/borders.
4. Suppress the old FULL edge-row emission only when the compiled run-edge path succeeded completely.
5. On missing selector entries, saturation, unsupported family/profile, or executor fault, use the old edge path unchanged.

This keeps persistent ownership, dirty/name-table bookkeeping and interior semantics in the proven GG machinery while replacing the repeated FULL edge derivation with the target architecture.

## Required A/B

Baseline: current exact-ROM parity materializer generated by `tools/build_parity_materializer.py`.

Candidate: same generated materializer plus the FULL run-edge finite-program executor.

Both variants must run the same deterministic turn and forward traces. Compare:

1. logical-update hash equality;
2. visible 20x18 name-table equality;
3. executor coverage and fallback rate;
4. cells selected, ownership-rejected and offscreen cells;
5. executor setup, sparse dispatch, bank switching, ownership gate and playback cycles separately;
6. materializer and whole-renderer cycles;
7. ROM bytes, WRAM bytes and bank placement.

## Decision ladder

RUNG A: prove banked sparse dispatch + FULL run-edge body playback exactly in a real GG semantic-probe ROM.

RUNG B: integrate FULL run-edge playback before ownership claiming, using old edge emission as exact fallback. This is the first playable-renderer architectural A/B.

RUNG C: add the one-byte-per-cell ownership tag and remove any need for a 20x32 live guard name table; compare its ROM growth against the saved WRAM and gate cost.

RUNG D: hoist family/step threshold and descriptor bases to run-edge scope. The Z80 target measured this as 52,266,733 -> 50,479,935 T over its edge corpus, a 3.42% edge-path saving.

RUNG E: attack the remaining dispatcher. The post-hoist Z80 profile is 697.7 T/chunk, dominated by rank scan, block entry and descriptor/base work. Do not assume those costs transfer unchanged to the Game Gear; measure them in the ROM executor.

RUNG F: compress the proven live vocabulary toward the shipping ROM budget. Do not sacrifice semantic/profiling clarity merely to make the first experiment fit the current 128 KiB image.

## Decision rule

KEEP a rung only when it is exact and its target-ROM cycle/space tradeoff is favourable. A slower first semantic executor is not automatically a rejection of the architecture: first determine whether the loss is executor playback, sparse dispatch, ownership integration, bank switching, or table access. Reject the architecture only after those costs are separated.

This experiment closes the real architectural question: whether the Game Gear can adopt the Z80 target's compiled **run-edge program** model, rather than merely making the old surface-column renderer increasingly clever.
