# Polar hardware-fit recovery branch

Branch: `feature/gg-tilesector-polar-field--hardware-fit-recovery`

Starting point: `e520388a` / Polar Run 81, fully green.

## Anchor measurement

Mode 0 raw: **588,886 T/update = 6.079 updates/s**.
Profiled render/build: **529,848 T/update**.
VRAM upload: **16,398 T/update**.
Transmitted dirty words: **110.5/update**.

Historical pre-carry-over speed anchor: approximately **567,991 T/update = 6.302 updates/s**.
That older number is useful only as a speed comparator; the current branch has restored persistent name-table lifetime, direct dirty production, the visual oracle, and near-first ownership semantics.

## Mission

Do not continue as an open-ended micro-optimization pass.

The target machine is:

- PC baker performs geometry/topology reasoning.
- Cartridge ROM stores the answer or the smallest local correction needed to recover it.
- Z80 selects, adds/steps/lookups, and emits final GG name-table words.
- WRAM remembers the authoritative desired name table and minimal lifetime state.
- VDP consumes horizontal name-table transactions.

Every optimization must say which runtime class of work it deletes. If it merely makes work cheaper that the next planned representation should remove, it is secondary.

## Mature TileSector lessons to port deliberately

1. **Stage 20 row extents:** geometry ownership is naturally column-major, but VDP dirtiness is naturally row-major. Test direct per-row first/last dirty extents instead of generating a 54-byte dirty bitset and decoding first/last during VBlank.
2. **Stage 21 FULL symmetry / VFLIP:** Polar currently solves and LUT-materializes top and bottom edges separately. Re-test the old exact-mirror convention for FULL walls before accepting this duplication. Do not assume current y=72 quantization is VFLIP-exact; prove or deliberately change the FULL quantization convention and compare visual error.
3. **Stage 22/23 lesson moved upstream:** retained work only matters if the test occurs before expensive projection/materialization. The eventual Polar version should skip an unchanged baked run before `project_key` / run geometry, not merely avoid a final store.
4. **Final-owner before store:** near-first ownership is correct in spirit, but per-row ownership queries are now part of the 252K materializer hotspot. Measure saved hidden rows versus ownership-check cost. Prefer baked/run-level visibility intervals over paying a row query for every possible store.
5. **Start + step everywhere:** preserve one corner calculation per unique corner and one wall-distance calculation per span. Next target is cell-local baked bearing/range corrections so runtime projection becomes loads + adds + lookup, deleting generic math rather than optimizing it.
6. **Runtime ordering is a bake-boundary problem:** current recipe order is not universally valid. Replace insertion sort only when the bake represents order-change event boundaries or a tiny baked permutation selector.
7. **Do not trust helper names in profiles:** linked symbols are authoritative. Current real generic multiply target is `__mul16`, about 33K T/update; `__mullong` is not linked.
8. **Two correctness gates:** Polar-to-Polar optimizations must preserve exact map hashes. Polar-to-legacy differences need semantic/first-divergence classification; absence of geometry is not dismissed as cosmetic.

## Accepted results

### A. Stage20-shaped row dirty extents - ACCEPTED

Run 83 is fully green and preserves the exact Polar map hash plus the same legacy first-divergence location.

- Raw Mode 0: **588,886 -> 581,424 T/update** (-7,462 T, **-1.27%**).
- Update rate: **6.079 -> 6.157 updates/s**.
- Render/build: **529,848 -> 526,230 T/update** (-3,618 T).
- VRAM upload: **16,398 -> 11,827 T/update** (-4,571 T, **-27.9%**).
- Transmitted words remain **110.5/update**: this is bookkeeping/addressing savings, not a changed transfer policy.
- WRAM dirty metadata: **54-byte bitset -> 36 bytes of row first/last extents**.

This is exactly the old Stage20 lesson: VDP dirtiness should already be stored in the shape consumed by the horizontal transaction.

## Priority experiments

A. **Stage20-shaped row extent A/B** - DONE / ACCEPTED.

B. **FULL VFLIP/symmetry A/B** - specifically target duplicated top/bottom edge work in the 252K materializer hotspot. Must prove the geometry convention before adopting.

C. **Ownership cost audit** - count full-span rejects, per-row rejects, and ownership checks. If rejection rate does not justify checks, replace the representation rather than polish the helper.

D. **Adaptive local projection bake** - primary architecture step. Bake local corner bearing/range base + correction data, refine spatial cells at discontinuities, and aim to delete most of `project_key`, `bearing_q12`, `inv_at_invd`, and surviving generic multiplies.

E. **Baked order-event representation** - treat runtime sort and overdraw as one missing-bake problem.

## Stop rule

No chain of small commits without a whole-frame anchor. After each experiment report:

- raw T/update and updates/s;
- render/build and upload;
- exact Polar map-hash result;
- what class of work disappeared;
- whether the result supports or falsifies the hypothesis.

Twenty unique rendered updates/s remains about **179K T/update**. Work that cannot plausibly move us toward that scale is support work, not the main campaign.
