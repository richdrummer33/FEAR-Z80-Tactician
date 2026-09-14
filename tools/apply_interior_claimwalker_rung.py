#!/usr/bin/env python3
# Exact-ROM integration rung: preserve near-to-far ownership, cheapen sequential interior tests.
from pathlib import Path

p = Path('src/tilesector_polar_materialize_gg.s')
s = p.read_text()

# Two bytes of persistent state for the sequential interior-row ownership test.
data_old = '''r_full_tile$:
        .ds     1
r_cov_first$:
'''
data_new = '''r_full_tile$:
        .ds     1
r_fill_claim_mask$:
        .ds     1
r_fill_claim_byte$:
        .ds     1
r_cov_first$:
'''
assert s.count(data_old) == 1
s = s.replace(data_old, data_new, 1)

# Initialize ownership-walker state BEFORE map_ptr_row_col$ so HL is scratch
# during setup. Only after setup do we establish HL as the persistent RAM
# name-table pointer. Reload A=first row immediately before map_ptr_row_col$.
init_old = '''        ld      a, (#r_fill_first$)
        ld      (#r_row$), a
        call    map_ptr_row_col$
        call    full_tile_low$
        ld      (#r_full_tile$), a
interior_loop$:
        push    hl
        ld      a, (#r_row$)
        call    polar_row_unclaimed_fast$
_tsp_polar_p_fill::
        pop     hl
        jr      z, polar_interior_done$
'''
init_new = '''        ld      a, (#r_fill_first$)
        ld      (#r_row$), a

        ; CLAIMWALK_A: the interior visits monotonically increasing rows.
        ; Resolve the starting bit and one of r_unclaimed0/1/2 once, then
        ; carry both through the loop instead of rebuilding them per row.
        and     #7
        ld      l, a
        ld      h, #0
        ld      de, #polar_dirty_mask_lut$
        add     hl, de
        ld      a, (hl)
        ld      (#r_fill_claim_mask$), a
        ld      a, (#r_fill_first$)
        cp      #8
        jr      c, interior_claim_init_g0$
        cp      #16
        jr      c, interior_claim_init_g1$
        ld      a, (#r_unclaimed2$)
        jr      interior_claim_init_store$
interior_claim_init_g1$:
        ld      a, (#r_unclaimed1$)
        jr      interior_claim_init_store$
interior_claim_init_g0$:
        ld      a, (#r_unclaimed0$)
interior_claim_init_store$:
        ld      (#r_fill_claim_byte$), a

        ; Establish the persistent name-table pointer only after walker setup.
        ld      a, (#r_fill_first$)
        call    map_ptr_row_col$
        call    full_tile_low$
        ld      (#r_full_tile$), a
interior_loop$:
        ld      a, (#r_fill_claim_mask$)
        ld      e, a
        ld      a, (#r_fill_claim_byte$)
        and     e
_tsp_polar_p_fill::
        jr      z, polar_interior_done$
'''
assert s.count(init_old) == 1
s = s.replace(init_old, init_new, 1)

# Advance the RAM name-table pointer, row and ownership bit together. Avoid
# doing any of that after the final interior row.
tail_old = '''polar_interior_done$:
        ld      de, #40              ; next RAM name-table row, same column
        add     hl, de
        ld      a, (#r_row$)
        inc     a
        ld      (#r_row$), a
        dec     c
        jr      nz, interior_loop$
        ret
'''
tail_new = '''polar_interior_done$:
        dec     c
        ret     z

        ld      de, #40              ; next RAM name-table row, same column
        add     hl, de
        ld      a, (#r_row$)
        inc     a
        ld      (#r_row$), a
        ld      e, a                  ; E=new row for the rare byte crossing

        ld      a, (#r_fill_claim_mask$)
        add     a, a
        jr      nz, interior_claim_mask_store$
        ld      a, #1
        ld      (#r_fill_claim_mask$), a
        ld      a, e
        cp      #8
        jr      z, interior_claim_load_g1$
        ; The only other in-range wrap is row 16.
        ld      a, (#r_unclaimed2$)
        ld      (#r_fill_claim_byte$), a
        jr      interior_loop$
interior_claim_load_g1$:
        ld      a, (#r_unclaimed1$)
        ld      (#r_fill_claim_byte$), a
        jr      interior_loop$
interior_claim_mask_store$:
        ld      (#r_fill_claim_mask$), a
        jr      interior_loop$
'''
assert s.count(tail_old) == 1
s = s.replace(tail_old, tail_new, 1)

p.write_text(s)
print('INTEGRATED_MATERIALIZER_RUNG=INTERIOR_CLAIMWALK_A')
