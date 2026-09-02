        .title  "Doomguy playable dual name-table uploader"
        .module doomguy_playable_ntupload_gg

        .area _HOME
        .globl _g_map

; Each entry point writes one nine-row half of the 20x18 visible window into
; an INACTIVE 32x28 name table. A half is 360 bytes, comfortably below one
; Game Gear VBlank. The caller flips VDP R2 only after the second half exists.

_doom_play_nt_upload_3000_top::
        ld      hl, #_g_map
        ld      iy, #nt_rows_3000$
        jr      nt_upload_half$

_doom_play_nt_upload_3000_bottom::
        ld      hl, #_g_map+360
        ld      iy, #nt_rows_3000$+18
        jr      nt_upload_half$

_doom_play_nt_upload_3800_top::
        ld      hl, #_g_map
        ld      iy, #nt_rows_3800$
        jr      nt_upload_half$

_doom_play_nt_upload_3800_bottom::
        ld      hl, #_g_map+360
        ld      iy, #nt_rows_3800$+18

nt_upload_half$:
        push    af
        push    bc
        push    de
        push    hl
        push    iy

        ld      a, #9
        ld      (#nt_rows_left$), a
nt_row_loop$:
        ld      e, 0 (iy)
        ld      d, 1 (iy)
        inc     iy
        inc     iy

        di
        ld      a, e
        out     (#0xBF), a
        ld      a, d
        or      #0x40
        out     (#0xBF), a
        ld      c, #0xBE
        ld      b, #40
        otir
        ei

        ld      a, (#nt_rows_left$)
        dec     a
        ld      (#nt_rows_left$), a
        jr      nz, nt_row_loop$

        pop     iy
        pop     hl
        pop     de
        pop     bc
        pop     af
        ret

        .area _CODE
; Visible 20-column window begins at column six, row three.
nt_rows_3000$:
        .dw 0x30CC,0x310C,0x314C,0x318C,0x31CC,0x320C,0x324C,0x328C,0x32CC
        .dw 0x330C,0x334C,0x338C,0x33CC,0x340C,0x344C,0x348C,0x34CC,0x350C
nt_rows_3800$:
        .dw 0x38CC,0x390C,0x394C,0x398C,0x39CC,0x3A0C,0x3A4C,0x3A8C,0x3ACC
        .dw 0x3B0C,0x3B4C,0x3B8C,0x3BCC,0x3C0C,0x3C4C,0x3C8C,0x3CCC,0x3D0C

        .area _DATA
nt_rows_left$: .ds 1
