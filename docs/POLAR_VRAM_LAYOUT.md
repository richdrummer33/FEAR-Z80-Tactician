# Polar Game Gear VRAM layout invariant

## The bug this records

GBDK 4.5.0's SMS/GG CRT initializes VDP register R2 with the pattern-name table
at **0x1800**. That layout leaves only:

- 0x0000..0x17FF for background patterns
- 192 complete 32-byte 4bpp tile patterns

The Polar renderer's generic hardware vocabulary is **423 tile IDs**. The baked
sub-tile compositor can also legitimately allocate more than 192 persistent
patterns over time.

Therefore the old combination was internally inconsistent:

1. load background tile IDs >= 192;
2. those 32-byte pattern writes land at/above 0x1800;
3. the same VRAM addresses are also the pattern-name table;
4. name-table uploads then overwrite those patterns, while later pattern writes
   overwrite name-table words.

The visual fingerprint was tile-aligned glyph/sliver corruption even though the
host/C oracle was geometrically clean.

## Locked Polar layout

Every Polar Game Gear ROM must select the name table at **0x3800** before
loading tile patterns or uploading name words:

    __WRITE_VDP_REG(VDP_R2, R2_MAP_0x3800);

The Polar visible-row uploader uses the Game Gear LCD crop within the 32x28
name table. Its row starts are therefore:

    0x38CC, 0x390C, 0x394C, 0x398C, 0x39CC, 0x3A0C
    0x3A4C, 0x3A8C, 0x3ACC, 0x3B0C, 0x3B4C, 0x3B8C
    0x3BCC, 0x3C0C, 0x3C4C, 0x3C8C, 0x3CCC, 0x3D0C

This yields Sega's standard maximum background-pattern area:

- 0x0000..0x37FF = 448 complete 32-byte tile patterns, IDs 0..447
- 0x3800..0x3EFF = 32x28 pattern-name table
- 0x3F00.. = sprite attribute table

The 423-entry generic Polar vocabulary now fits by construction.

## Regression evidence

Branch: `feature/gg-polar-baked-edge-composite`

The host-baked compositor and actual Gearsystem Game Gear framebuffer are
semantic-pixel exact at logical patches 120, 300, 520, 760, 1000 and 1113:

    semantic_pixel_mismatch = 0 / 23040 at every sampled frame
    POLAR_COMPOSITE_SEMANTIC_PIXEL_EXACT = 1

The current baked exploration reports:

- peak unique patterns in one frame: 79
- original peak pattern loads in a transition: 39
- total pattern loads across the 1113-transition proof: 6578
- offline scheduled steady-state budget: 9 pattern uploads per update
- generated tile-pattern banks: 17

A manual-takeover ROM is also built and smoke-tested with deterministic RIGHT
input so the live generic renderer re-enters after the baked rails.

## Future sprite warning

Background tile IDs 256+ occupy VRAM at/above 0x2000. GBDK's SMS/GG sprite
pattern helper also addresses sprite pattern data in that high pattern region.
The current Polar proof hides sprites, so this is not a present conflict.

Before restoring hardware actor/projectile sprites, explicitly partition the
pattern region or reduce/remap the background vocabulary. Do not silently assume
all 448 background slots and a full independent sprite-pattern bank can coexist.

## Carry-forward rule

Treat the R2=0x3800 selection and 0x38CC-based Polar upload tables as one
invariant. Changing one without the other recreates the same class of bug.
