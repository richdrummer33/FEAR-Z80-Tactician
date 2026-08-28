        .title  "Polar GG fast materializer"
        .module tilesector_polar_materialize_gg

        .area   _HOME

        .globl  _g_polar_mat_col
        .globl  _g_polar_mat_shade
        .globl  _g_polar_mat_border
        .globl  _g_polar_mat_top_l
        .globl  _g_polar_mat_top_r
        .globl  _g_polar_mat_bot_l
        .globl  _g_polar_mat_bot_r
        .globl  _g_map

; Explicit polar materializer bridge. No C struct offsets and no argument-register
; convention: every input is a named symbol, and the visible aperture is always
; the full 18-row GG viewport.

_tsp_polar_surface_column_fast::
        push    bc
        push    de
        push    hl
        ld      a, (#_g_polar_mat_col)
        ld      b, a
        xor     a
        ld      (#r_clip_first$), a
        ld      a, #17
        ld      (#r_clip_last$), a

        ; Four signed pixel endpoints -> four signed tile rows.
        ld      hl, (#_g_polar_mat_top_l)
        call    row_floor_hl$
        ld      (#r_top_l_row$), a
        ld      hl, (#_g_polar_mat_top_r)
        call    row_floor_hl$
        ld      (#r_top_r_row$), a
        ld      hl, (#_g_polar_mat_bot_l)
        call    row_floor_hl$
        ld      (#r_bot_l_row$), a
        ld      hl, (#_g_polar_mat_bot_r)
        call    row_floor_hl$
        ld      (#r_bot_r_row$), a

        ; Signed min/max for top endpoints.
        ld      a, (#r_top_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#r_top_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jr      c, top_r_is_min$
        ld      a, e
        ld      (#r_top_min$), a
        ld      a, d
        ld      (#r_top_max$), a
        jr      top_minmax_done$
top_r_is_min$:
        ld      a, d
        ld      (#r_top_min$), a
        ld      a, e
        ld      (#r_top_max$), a
top_minmax_done$:

        ; Signed min/max for bottom endpoints.
        ld      a, (#r_bot_l_row$)
        ld      e, a
        xor     #0x80
        ld      c, a
        ld      a, (#r_bot_r_row$)
        ld      d, a
        xor     #0x80
        cp      c
        jr      c, bot_r_is_min$
        ld      a, e
        ld      (#r_bot_min$), a
        ld      a, d
        ld      (#r_bot_max$), a
        jr      bot_minmax_done$
bot_r_is_min$:
        ld      a, d
        ld      (#r_bot_min$), a
        ld      a, e
        ld      (#r_bot_max$), a
bot_minmax_done$:

        ; Polar runtime semantics: both visible boundaries are vector edges.
        ; LINTEL/RISER/RAISED only change projected endpoint Y upstream.
        xor     a
        call    prepare_edge$          ; top

        ld      a, #1
        call    prepare_edge$          ; bottom

        call    draw_plain_interior$
        jr      raster_done$

raster_done$:
        pop     hl
        pop     de
        pop     bc
        ret

; Signed floor(pixel/8). Arithmetic shifting gives true floor for negative Y.
row_floor_hl$:
        sra     h
        rr      l
        sra     h
        rr      l
        sra     h
        rr      l
        ld      a, l
        ret

; A=0 top / 1 bottom. Prepares original edge endpoints/slope then draws the
; one or two hardware-tile rows that a <=7px connected edge can cross.
prepare_edge$:
        ld      (#r_edge_bottom$), a
        or      a
        jr      nz, prep_bottom$
        ld      hl, (#_g_polar_mat_top_l)
        ld      (#r_edge_left$), hl
        ld      de, (#_g_polar_mat_top_r)
        ld      a, (#r_top_min$)
        ld      (#r_edge_min$), a
        ld      a, (#r_top_max$)
        ld      (#r_edge_max$), a
        jr      prep_slope$
prep_bottom$:
        ld      hl, (#_g_polar_mat_bot_l)
        ld      (#r_edge_left$), hl
        ld      de, (#_g_polar_mat_bot_r)
        ld      a, (#r_bot_min$)
        ld      (#r_edge_min$), a
        ld      a, (#r_bot_max$)
        ld      (#r_edge_max$), a
prep_slope$:
        ex      de, hl                  ; HL=right, DE=left
        or      a
        sbc     hl, de
        ld      a, l                    ; signed slope, clamp exactly like C
        bit     7, a
        jr      nz, slope_negative$
        cp      #8
        jr      c, slope_store$
        ld      a, #7
        jr      slope_store$
slope_negative$:
        cp      #0xF9                  ; -7
        jr      nc, slope_store$
        ld      a, #0xF9
slope_store$:
        ld      (#r_edge_slope$), a
        ; Polar path may cross more than two tile rows at steep/near
        ; perspective. Match the C oracle: draw every row from min..max while
        ; using the clamped [-7,+7] edge slope for tile selection.
        ld      a, (#r_edge_min$)
edge_rows_loop$:
        ld      (#r_edge_iter$), a
        call    draw_edge_row$
        ld      a, (#r_edge_iter$)
        ld      c, a
        ld      a, (#r_edge_max$)
        cp      c
        ret     z
        ld      a, c
        inc     a
        jr      edge_rows_loop$

; A=signed row. Reject offscreen/occluded rows, then table-lookup the exact
; edge tile+flip attributes and write it into g_map.
draw_edge_row$:
        ld      (#r_row$), a
        bit     7, a
        ret     nz
        cp      #18
        ret     nc
        ld      c, a
        ld      a, (#r_clip_first$)
        cp      c
        jr      c, edge_after_first$
        jr      z, edge_after_first$
        ret
edge_after_first$:
        ld      a, (#r_clip_last$)
        cp      c
        ret     c

        ; local = left_y - row*8; low-byte arithmetic is exact in this range.
        ld      a, c
        add     a, a
        add     a, a
        add     a, a
        ld      e, a
        ld      a, (#r_edge_left$)
        sub     e
        ; Conservative clamp into LUT local domain [-15,+15].
        cp      #0x80
        jr      c, local_positive$
        cp      #0xF1                  ; -15
        jr      nc, local_ready$
        ld      a, #0xF1
        jr      local_ready$
local_positive$:
        cp      #16
        jr      c, local_ready$
        ld      a, #15
local_ready$:
        add     a, #15
        ld      (#r_local_index$), a

        ; group = slope+7 + (bottom?15:0); index = group*31 + local_index.
        ld      a, (#r_edge_slope$)
        add     a, #7
        ld      e, a
        ld      a, (#r_edge_bottom$)
        or      a
        jr      z, group_ready$
        ld      a, e
        add     a, #15
        ld      e, a
group_ready$:
        ld      l, e
        ld      h, #0
        ld      d, #0
        push    de                    ; group
        add     hl, hl                ; 2
        add     hl, hl                ; 4
        add     hl, hl                ; 8
        add     hl, hl                ; 16
        add     hl, hl                ; 32
        pop     de
        or      a
        sbc     hl, de                ; 31*group
        ld      a, (#r_local_index$)
        ld      e, a
        ld      d, #0
        add     hl, de
        add     hl, hl
        ld      de, #edge_lut$
        add     hl, de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)

        ; LUT is shade zero. Add 0x0080/0x0100 for mid/near families.
        ld      a, (#_g_polar_mat_shade)
        or      a
        jr      z, edge_tile_ready$
        dec     a
        jr      nz, edge_shade_two$
        ld      a, e
        add     a, #0x80
        ld      e, a
        jr      nc, edge_tile_ready$
        inc     d
        jr      edge_tile_ready$
edge_shade_two$:
        inc     d
edge_tile_ready$:
        push    de
        ld      a, (#r_row$)
        call    map_ptr_row_col$
        pop     de
        ld      (hl), e
        inc     hl
        ld      (hl), d
        ld      a, (#r_row$)
        ret

; A=signed row, E=cap delta (4 top, 8 bottom).
draw_full_single$:
        ld      (#r_row$), a
        ld      a, e
        ld      (#r_cap_delta$), a
        ld      a, (#r_row$)
        bit     7, a
        ret     nz
        cp      #18
        ret     nc
        ld      c, a
        ld      a, (#r_clip_first$)
        cp      c
        jr      z, full_first_ok$
        jr      c, full_first_ok$
        ret
full_first_ok$:
        ld      a, (#r_clip_last$)
        cp      c
        ret     c
        ld      a, (#r_row$)
        call    map_ptr_row_col$
        call    full_tile_low$
        ld      e, a
        ld      a, (#r_cap_delta$)
        add     a, e
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        ret

; Fill top_max+1 .. bot_min-1, clipped to the portal aperture.
draw_plain_interior$:
        ld      a, (#r_top_max$)
        inc     a
        bit     7, a
        jr      nz, interior_first_clip$
        ld      c, a
        ld      a, (#r_clip_first$)
        cp      c
        jr      c, interior_first_keep$
        jr      z, interior_first_keep$
interior_first_clip$:
        ld      a, (#r_clip_first$)
        ld      c, a
interior_first_keep$:
        ld      a, c
        cp      #18
        ret     nc
        ld      (#r_fill_first$), a

        ld      a, (#r_bot_min$)
        dec     a
        bit     7, a
        ret     nz
        ld      c, a
        ld      a, (#r_clip_last$)
        cp      c
        jr      nc, interior_last_keep$
        ld      c, a
interior_last_keep$:
        ld      a, c
        cp      #18
        jr      c, interior_last_valid$
        ld      a, #17
        ld      c, a
interior_last_valid$:
        ld      a, (#r_fill_first$)
        cp      c
        jr      c, interior_multi$
        jr      z, interior_multi$
        ret
interior_multi$:
        ; count=last-first+1
        ld      a, c
        ld      e, a
        ld      a, (#r_fill_first$)
        ld      d, a
        ld      a, e
        sub     d
        inc     a
        ld      c, a
        ld      a, (#r_fill_first$)
        call    map_ptr_row_col$
        call    full_tile_low$
        ld      (#r_full_tile$), a
        ld      de, #39              ; after high byte, +39 => next row low
interior_loop$:
        ld      a, (#r_full_tile$)
        ld      (hl), a
        inc     hl
        ld      (hl), #0
        add     hl, de
        dec     c
        jr      nz, interior_loop$
        ret

; Return low-byte full tile ID for current shade/border, cap none.
full_tile_low$:
        ld      a, (#_g_polar_mat_shade)
        or      a
        jr      z, full_far$
        dec     a
        jr      z, full_mid$
        ld      a, #27
        jr      full_add_border$
full_mid$:
        ld      a, #15
        jr      full_add_border$
full_far$:
        ld      a, #3
full_add_border$:
        ld      e, a
        ld      a, (#_g_polar_mat_border)
        add     a, e
        ret

; A=row 0..17, B=column 0..19 -> HL=&g_map[row*20+col].
map_ptr_row_col$:
        ld      l, a
        ld      h, #0
        add     hl, hl                ; 2r
        add     hl, hl                ; 4r
        add     hl, hl                ; 8r
        ld      d, h
        ld      e, l                  ; DE=8r
        add     hl, hl                ; 16r
        add     hl, hl                ; 32r
        add     hl, de                ; 40r bytes
        ld      a, b
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        ld      de, #_g_map
        add     hl, de
        ret

; edge_lut[bottom][slope+7][local+15], shade-zero tile word.
; Each entry already contains H/V flip + palette attributes exactly as the
; C edge_entry() path; the assembly kernel only adds the shade tile offset.
edge_lut$:
        ; bottom=0 slope=-7
        .dw 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E
        .dw 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E
        .dw 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x022E
        .dw 0x022E, 0x022E, 0x022E, 0x022E, 0x022E, 0x0236, 0x023E
        ; bottom=0 slope=-6
        .dw 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D
        .dw 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D
        .dw 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D, 0x022D
        .dw 0x022D, 0x022D, 0x022D, 0x022D, 0x0235, 0x023D, 0x0245
        ; bottom=0 slope=-5
        .dw 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C
        .dw 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C
        .dw 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C, 0x022C
        .dw 0x022C, 0x022C, 0x022C, 0x0234, 0x023C, 0x0244, 0x024C
        ; bottom=0 slope=-4
        .dw 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B
        .dw 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B
        .dw 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B, 0x022B
        .dw 0x022B, 0x022B, 0x0233, 0x023B, 0x0243, 0x024B, 0x0253
        ; bottom=0 slope=-3
        .dw 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A
        .dw 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A
        .dw 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A, 0x022A
        .dw 0x022A, 0x0232, 0x023A, 0x0242, 0x024A, 0x0252, 0x025A
        ; bottom=0 slope=-2
        .dw 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229
        .dw 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229
        .dw 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229, 0x0229
        .dw 0x0231, 0x0239, 0x0241, 0x0249, 0x0251, 0x0259, 0x0261
        ; bottom=0 slope=-1
        .dw 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228
        .dw 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228
        .dw 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0228, 0x0230
        .dw 0x0238, 0x0240, 0x0248, 0x0250, 0x0258, 0x0260, 0x0268
        ; bottom=0 slope=+0
        .dw 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027
        .dw 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027, 0x0027
        .dw 0x002F, 0x0037, 0x003F, 0x0047, 0x004F, 0x0057, 0x005F, 0x0067
        .dw 0x006F, 0x0077, 0x007F, 0x0087, 0x008F, 0x0097, 0x009F
        ; bottom=0 slope=+1
        .dw 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028
        .dw 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028, 0x0028
        .dw 0x0030, 0x0038, 0x0040, 0x0048, 0x0050, 0x0058, 0x0060, 0x0068
        .dw 0x0070, 0x0078, 0x0080, 0x0088, 0x0090, 0x0098, 0x00A0
        ; bottom=0 slope=+2
        .dw 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029
        .dw 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029, 0x0029
        .dw 0x0031, 0x0039, 0x0041, 0x0049, 0x0051, 0x0059, 0x0061, 0x0069
        .dw 0x0071, 0x0079, 0x0081, 0x0089, 0x0091, 0x0099, 0x00A1
        ; bottom=0 slope=+3
        .dw 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A
        .dw 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A, 0x002A
        .dw 0x0032, 0x003A, 0x0042, 0x004A, 0x0052, 0x005A, 0x0062, 0x006A
        .dw 0x0072, 0x007A, 0x0082, 0x008A, 0x0092, 0x009A, 0x00A2
        ; bottom=0 slope=+4
        .dw 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B
        .dw 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B, 0x002B
        .dw 0x0033, 0x003B, 0x0043, 0x004B, 0x0053, 0x005B, 0x0063, 0x006B
        .dw 0x0073, 0x007B, 0x0083, 0x008B, 0x0093, 0x009B, 0x00A3
        ; bottom=0 slope=+5
        .dw 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C
        .dw 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C, 0x002C
        .dw 0x0034, 0x003C, 0x0044, 0x004C, 0x0054, 0x005C, 0x0064, 0x006C
        .dw 0x0074, 0x007C, 0x0084, 0x008C, 0x0094, 0x009C, 0x00A4
        ; bottom=0 slope=+6
        .dw 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D
        .dw 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D, 0x002D
        .dw 0x0035, 0x003D, 0x0045, 0x004D, 0x0055, 0x005D, 0x0065, 0x006D
        .dw 0x0075, 0x007D, 0x0085, 0x008D, 0x0095, 0x009D, 0x00A5
        ; bottom=0 slope=+7
        .dw 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E
        .dw 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E, 0x002E
        .dw 0x0036, 0x003E, 0x0046, 0x004E, 0x0056, 0x005E, 0x0066, 0x006E
        .dw 0x0076, 0x007E, 0x0086, 0x008E, 0x0096, 0x009E, 0x00A6
        ; bottom=1 slope=-7
        .dw 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6, 0x0CA6
        .dw 0x0C9E, 0x0C96, 0x0C8E, 0x0C86, 0x0C7E, 0x0C76, 0x0C6E, 0x0C66
        .dw 0x0C5E, 0x0C56, 0x0C4E, 0x0C46, 0x0C3E, 0x0C36, 0x0C2E, 0x0C2E
        .dw 0x0C2E, 0x0C2E, 0x0C2E, 0x0C2E, 0x0C2E, 0x0C2E, 0x0C2E
        ; bottom=1 slope=-6
        .dw 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5, 0x0CA5
        .dw 0x0C9D, 0x0C95, 0x0C8D, 0x0C85, 0x0C7D, 0x0C75, 0x0C6D, 0x0C65
        .dw 0x0C5D, 0x0C55, 0x0C4D, 0x0C45, 0x0C3D, 0x0C35, 0x0C2D, 0x0C2D
        .dw 0x0C2D, 0x0C2D, 0x0C2D, 0x0C2D, 0x0C2D, 0x0C2D, 0x0C2D
        ; bottom=1 slope=-5
        .dw 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4, 0x0CA4
        .dw 0x0C9C, 0x0C94, 0x0C8C, 0x0C84, 0x0C7C, 0x0C74, 0x0C6C, 0x0C64
        .dw 0x0C5C, 0x0C54, 0x0C4C, 0x0C44, 0x0C3C, 0x0C34, 0x0C2C, 0x0C2C
        .dw 0x0C2C, 0x0C2C, 0x0C2C, 0x0C2C, 0x0C2C, 0x0C2C, 0x0C2C
        ; bottom=1 slope=-4
        .dw 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3, 0x0CA3
        .dw 0x0C9B, 0x0C93, 0x0C8B, 0x0C83, 0x0C7B, 0x0C73, 0x0C6B, 0x0C63
        .dw 0x0C5B, 0x0C53, 0x0C4B, 0x0C43, 0x0C3B, 0x0C33, 0x0C2B, 0x0C2B
        .dw 0x0C2B, 0x0C2B, 0x0C2B, 0x0C2B, 0x0C2B, 0x0C2B, 0x0C2B
        ; bottom=1 slope=-3
        .dw 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2, 0x0CA2
        .dw 0x0C9A, 0x0C92, 0x0C8A, 0x0C82, 0x0C7A, 0x0C72, 0x0C6A, 0x0C62
        .dw 0x0C5A, 0x0C52, 0x0C4A, 0x0C42, 0x0C3A, 0x0C32, 0x0C2A, 0x0C2A
        .dw 0x0C2A, 0x0C2A, 0x0C2A, 0x0C2A, 0x0C2A, 0x0C2A, 0x0C2A
        ; bottom=1 slope=-2
        .dw 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1, 0x0CA1
        .dw 0x0C99, 0x0C91, 0x0C89, 0x0C81, 0x0C79, 0x0C71, 0x0C69, 0x0C61
        .dw 0x0C59, 0x0C51, 0x0C49, 0x0C41, 0x0C39, 0x0C31, 0x0C29, 0x0C29
        .dw 0x0C29, 0x0C29, 0x0C29, 0x0C29, 0x0C29, 0x0C29, 0x0C29
        ; bottom=1 slope=-1
        .dw 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0, 0x0CA0
        .dw 0x0C98, 0x0C90, 0x0C88, 0x0C80, 0x0C78, 0x0C70, 0x0C68, 0x0C60
        .dw 0x0C58, 0x0C50, 0x0C48, 0x0C40, 0x0C38, 0x0C30, 0x0C28, 0x0C28
        .dw 0x0C28, 0x0C28, 0x0C28, 0x0C28, 0x0C28, 0x0C28, 0x0C28
        ; bottom=1 slope=+0
        .dw 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F, 0x0C9F
        .dw 0x0C97, 0x0C8F, 0x0C87, 0x0C7F, 0x0C77, 0x0C6F, 0x0C67, 0x0C5F
        .dw 0x0C57, 0x0C4F, 0x0C47, 0x0C3F, 0x0C37, 0x0C2F, 0x0C27, 0x0C27
        .dw 0x0C27, 0x0C27, 0x0C27, 0x0C27, 0x0C27, 0x0C27, 0x0C27
        ; bottom=1 slope=+1
        .dw 0x0E68, 0x0E68, 0x0E68, 0x0E68, 0x0E68, 0x0E68, 0x0E68, 0x0E68
        .dw 0x0E68, 0x0E68, 0x0E68, 0x0E68, 0x0E68, 0x0E60, 0x0E58, 0x0E50
        .dw 0x0E48, 0x0E40, 0x0E38, 0x0E30, 0x0E28, 0x0E28, 0x0E28, 0x0E28
        .dw 0x0E28, 0x0E28, 0x0E28, 0x0E28, 0x0E28, 0x0E28, 0x0E28
        ; bottom=1 slope=+2
        .dw 0x0E61, 0x0E61, 0x0E61, 0x0E61, 0x0E61, 0x0E61, 0x0E61, 0x0E61
        .dw 0x0E61, 0x0E61, 0x0E61, 0x0E61, 0x0E59, 0x0E51, 0x0E49, 0x0E41
        .dw 0x0E39, 0x0E31, 0x0E29, 0x0E29, 0x0E29, 0x0E29, 0x0E29, 0x0E29
        .dw 0x0E29, 0x0E29, 0x0E29, 0x0E29, 0x0E29, 0x0E29, 0x0E29
        ; bottom=1 slope=+3
        .dw 0x0E5A, 0x0E5A, 0x0E5A, 0x0E5A, 0x0E5A, 0x0E5A, 0x0E5A, 0x0E5A
        .dw 0x0E5A, 0x0E5A, 0x0E5A, 0x0E52, 0x0E4A, 0x0E42, 0x0E3A, 0x0E32
        .dw 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A
        .dw 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A, 0x0E2A
        ; bottom=1 slope=+4
        .dw 0x0E53, 0x0E53, 0x0E53, 0x0E53, 0x0E53, 0x0E53, 0x0E53, 0x0E53
        .dw 0x0E53, 0x0E53, 0x0E4B, 0x0E43, 0x0E3B, 0x0E33, 0x0E2B, 0x0E2B
        .dw 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B
        .dw 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B, 0x0E2B
        ; bottom=1 slope=+5
        .dw 0x0E4C, 0x0E4C, 0x0E4C, 0x0E4C, 0x0E4C, 0x0E4C, 0x0E4C, 0x0E4C
        .dw 0x0E4C, 0x0E44, 0x0E3C, 0x0E34, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C
        .dw 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C
        .dw 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C, 0x0E2C
        ; bottom=1 slope=+6
        .dw 0x0E45, 0x0E45, 0x0E45, 0x0E45, 0x0E45, 0x0E45, 0x0E45, 0x0E45
        .dw 0x0E3D, 0x0E35, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D
        .dw 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D
        .dw 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D, 0x0E2D
        ; bottom=1 slope=+7
        .dw 0x0E3E, 0x0E3E, 0x0E3E, 0x0E3E, 0x0E3E, 0x0E3E, 0x0E3E, 0x0E36
        .dw 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E
        .dw 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E
        .dw 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E, 0x0E2E

        .area _DATA
r_clip_first$:
        .ds     1
r_clip_last$:
        .ds     1
r_top_l_row$:
        .ds     1
r_top_r_row$:
        .ds     1
r_bot_l_row$:
        .ds     1
r_bot_r_row$:
        .ds     1
r_top_min$:
        .ds     1
r_top_max$:
        .ds     1
r_bot_min$:
        .ds     1
r_bot_max$:
        .ds     1
r_edge_left$:
        .ds     2
r_edge_slope$:
        .ds     1
r_edge_bottom$:
        .ds     1
r_edge_min$:
        .ds     1
r_edge_max$:
        .ds     1
r_edge_iter$:
        .ds     1
r_row$:
        .ds     1
r_local_index$:
        .ds     1
r_cap_delta$:
        .ds     1
r_fill_first$:
        .ds     1
r_full_tile$:
        .ds     1
