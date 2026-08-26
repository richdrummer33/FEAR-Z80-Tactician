# TileSector Stage 16 — direct opaque materializer

Stage 16 tests the next representation collapse after the authoritative name-table shadow work.

For `FULL` and `RAISED_FULL` surfaces, the Stage-12 horizontal run kernel no longer calls the generic portal-capable column raster. Those opaque profiles now route to a private GG materializer that:

- consumes the connected projected top/bottom endpoints already produced by the run kernel;
- reuses the existing preloaded vector-edge LUT instead of generating dynamic patterns;
- keeps the contiguous interior name-table pointer and dirty-bit pointer live while descending a screen column;
- stamps Stage-15 generation metadata directly into the authoritative RAM name-table shadow;
- marks the 54-byte dirty bitmap directly only when a visible name-table word changes; and
- closes the portal aperture immediately after the opaque column is emitted.

`LINTEL` and `RISER` deliberately remain on the generic portal-aware raster path so the first measurement isolates the common opaque case.

Reference high-water mark before this experiment: Stage 12 at approximately **7.93 complete world updates/s**. Stage 14 measured approximately **7.53 updates/s** while proving the persistent name-table/VBlank representation reduced upload cost to roughly **27.4K T/update**.

The purpose of Stage 16 is to determine whether collapsing `run -> generic column raster -> name-table cells` into `run -> opaque name-table materializer` produces the first large raster-side step after the representation changes.
