#!/usr/bin/env python3
from pathlib import Path
p=Path('src/tilesector_ntstate_gg.s')
s=p.read_text()
old='''        ld      a, (de)\n        ld      (#nte_dirty_old$), a\n        ld      a, (#nte_mask$)\n        ld      c, a\n        ld      a, (#nte_dirty_old$)\n        or      c\n        ld      (de), a\n        ; C is immediately rebuilt below at group boundaries; preserve the\n        ; within-group countdown explicitly instead of relying on it here.\n        ld      a, (#nte_group_left$)\n        ld      c, a\n\nnte_cell_done$:\n        inc     hl\n        inc     hl\n        ld      a, (#nte_mask$)\n        add     a, a\n        ld      (#nte_mask$), a\n        dec     c\n        ld      a, c\n        ld      (#nte_group_left$), a\n        jr      nz, nte_group_not_done$\n        inc     de\n        ld      a, #1\n        ld      (#nte_mask$), a\n        ld      c, #8\n        ld      a, b\n        cp      #4                     ; after 16 cells, final group has four\n        jr      nz, nte_group_reset$\n        ld      c, #4\nnte_group_reset$:\n        ld      a, c\n        ld      (#nte_group_left$), a\nnte_group_not_done$:'''
new='''        push    bc                      ; preserve cell/group loop counters\n        ld      a, (de)\n        ld      c, a\n        ld      a, (#nte_mask$)\n        or      c\n        ld      (de), a\n        pop     bc\n\nnte_cell_done$:\n        inc     hl\n        inc     hl\n        ld      a, (#nte_mask$)\n        add     a, a\n        ld      (#nte_mask$), a\n        dec     c\n        jr      nz, nte_group_not_done$\n        inc     de\n        ld      a, #1\n        ld      (#nte_mask$), a\n        ld      c, #8\n        ld      a, b\n        cp      #5                     ; before djnz at cell 16, B is five\n        jr      nz, nte_group_not_done$\n        ld      c, #4\nnte_group_not_done$:'''
if old not in s:
    raise SystemExit('Stage 15 scan block not found')
s=s.replace(old,new,1)
s=s.replace('nte_group_left$:   .ds 1\n','')
s=s.replace('nte_dirty_old$:    .ds 1\n','')
p.write_text(s)
print('Fixed Stage 15 generation scan loop preservation/group transition.')
