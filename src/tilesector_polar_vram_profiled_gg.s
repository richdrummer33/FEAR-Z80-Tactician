        .title  "Polar GG row-burst name-table upload"
        .module tilesector_polar_vram_profiled_gg

        .area   _HOME

        .globl  _g_map
        .globl  _g_prev_map
        .globl  _g_ts_dirty_words

; Polar row-burst uploader.
;
; Each frame still compares the authoritative 20x18 RAM name table with the
; previous-frame shadow. Instead of paying a fresh VDP address transaction for
; every changed word, each visible row records its first and last changed
; column. The complete [first..last] interval is then streamed with one OTIR.
; Clean words between dirty islands may be rewritten deliberately; on a
; 20-column GG viewport this trades a few cheap data writes for many avoided
; VDP control transactions.
;
; g_prev_map is updated only for genuinely changed words during the scan, so
; the next frame's dirty detection remains exact.
_ts_upload_dirty_map_fast::
        push    ix
        push    bc
        push    de
        push    hl

        xor     a
        ld      (_g_ts_dirty_words), a
        ld      (_g_ts_dirty_words+1), a
        ld      hl, #_g_map
        ld      de, #_g_prev_map
        ld      ix, #0x18CC
        ld      a, #18
        ld      (#rb_rows_left$), a

rb_row_loop$:
        ld      (#rb_row_start$), hl
        ld      a, #0xFF
        ld      (#rb_first$), a
        xor     a
        ld      (#rb_last$), a
        ld      (#rb_col$), a

rb_word_loop$:
        ; Compare current and previous 16-bit name-table word.
        ld      a, (de)
        cp      (hl)
        jr      nz, rb_changed$
        inc     de
        inc     hl
        ld      a, (de)
        cp      (hl)
        jr      nz, rb_changed_high$
        inc     de
        inc     hl
        jr      rb_word_done$

rb_changed_high$:
        dec     de
        dec     hl

rb_changed$:
        ; first = first changed column; last = most recent changed column.
        ld      a, (#rb_col$)
        ld      c, a
        ld      a, (#rb_first$)
        cp      #0xFF
        jr      nz, rb_first_known$
        ld      a, c
        ld      (#rb_first$), a
rb_first_known$:
        ld      a, c
        ld      (#rb_last$), a

        ; Copy the authoritative word into the previous-frame shadow.
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de
        ld      a, (hl)
        ld      (de), a
        inc     hl
        inc     de

        ; Profiler-visible count is the exact number of changed words, not
        ; deliberately bridged clean words inside the burst.
        ld      a, (_g_ts_dirty_words)
        inc     a
        ld      (_g_ts_dirty_words), a
        jr      nz, rb_word_done$
        ld      a, (_g_ts_dirty_words+1)
        inc     a
        ld      (_g_ts_dirty_words+1), a

rb_word_done$:
        ld      a, (#rb_col$)
        inc     a
        ld      (#rb_col$), a
        cp      #20
        jr      c, rb_word_loop$

        ; Preserve next-row RAM scan pointers while HL is reused as OTIR source
        ; and DE is reused for the VDP address calculation.
        push    hl
        push    de

        ld      a, (#rb_first$)
        cp      #0xFF
        jr      z, rb_no_burst$

        ; HL = current row start + first*2.
        ld      hl, (#rb_row_start$)
        add     a, a
        ld      e, a
        ld      d, #0
        add     hl, de
        push    hl

        ; DE = hardware name-table row base (IX) + first*2.
        push    ix
        pop     hl
        add     hl, de
        ex      de, hl
        pop     hl

        ; One VDP address setup, then stream all bytes first..last.
        di
        ld      a, e
        out     (#0xBF), a
        ld      a, d
        or      #0x40
        out     (#0xBF), a
        ld      a, (#rb_last$)
        ld      c, a
        ld      a, (#rb_first$)
        ld      b, a
        ld      a, c
        sub     b
        inc     a
        add     a, a
        ld      b, a
        ld      c, #0xBE
        otir
        ei

rb_no_burst$:
        pop     de
        pop     hl

        ; Next visible hardware row is +64 bytes; RAM rows are already
        ; contiguous because the scan pointers advanced by 40 bytes.
        push    de
        ld      de, #64
        add     ix, de
        pop     de

        ld      a, (#rb_rows_left$)
        dec     a
        ld      (#rb_rows_left$), a
        jp      nz, rb_row_loop$

        pop     hl
        pop     de
        pop     bc
        pop     ix
        ret

        .area _DATA
rb_row_start$:
        .ds     2
rb_rows_left$:
        .ds     1
rb_col$:
        .ds     1
rb_first$:
        .ds     1
rb_last$:
        .ds     1
