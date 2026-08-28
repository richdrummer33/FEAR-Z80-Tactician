        .title  "Adaptive Polar Field fast base-map reset"
        .module tilesector_polar_frame_gg

        .area   _HOME

        .globl  _g_map
        .globl  _tsp_polar_begin_map_fast

; Fresh polar-only frame reset.
;
; The polar renderer's desired name-table shadow is always a 20x18 contiguous
; RAM map. Instead of tracking/restoring last frame's touched cells, rebuild the
; static ceiling/horizon/floor template every update. 360 words are cheap enough
; that this is substantially simpler than lifetime bookkeeping on Z80.
;
; Rows 0..8  : ceiling tile 0
; Row  9     : horizon tile 2
; Rows 10..17: floor tile 1
;
; No arguments. This intentionally targets the polar GG entrypoint's _g_map.
_tsp_polar_begin_map_fast::
        push    bc
        push    de
        push    hl

        ld      hl, #_g_map

        ; 180 ceiling cells, word 0x0000.
        ld      d, #0
        ld      e, #0
        ld      b, #180
tspf_ceiling$:
        ld      (hl), d
        inc     hl
        ld      (hl), e
        inc     hl
        djnz    tspf_ceiling$

        ; 20 horizon cells, tile id 2 => word 0x0002.
        ld      d, #2
        ld      b, #20
tspf_horizon$:
        ld      (hl), d
        inc     hl
        ld      (hl), e
        inc     hl
        djnz    tspf_horizon$

        ; 160 floor cells, tile id 1 => word 0x0001.
        ld      d, #1
        ld      b, #160
tspf_floor$:
        ld      (hl), d
        inc     hl
        ld      (hl), e
        inc     hl
        djnz    tspf_floor$

        pop     hl
        pop     de
        pop     bc
        ret
