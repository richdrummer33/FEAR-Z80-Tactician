#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_core.c')
s=p.read_text()
old_sig='static void raster_surface_column(uint8_t col) {'
new_sig='static void raster_surface_column(uint8_t col, TSRasterCtx *ctx) {'
if new_sig in s:
    print('col,ctx raster ABI variant already materialized')
    raise SystemExit(0)
if old_sig not in s:
    raise SystemExit('persistent-context raster signature not found')
start=s.index(old_sig)
end=s.index('\n}\n\n/* Quantize one tile',start)+2
block=s[start:end]
block=block.replace(old_sig,new_sig,1)
block=block.replace('g_raster_ctx.','ctx->')
s=s[:start]+block+s[end:]
s=s.replace('raster_surface_column(c);','raster_surface_column(c,&g_raster_ctx);')
s=s.replace('raster_surface_column(uc);','raster_surface_column(uc,&g_raster_ctx);')
s=s.replace('''/* Persistent hot-path context. The pointer is deliberately not passed: under\n * Z80 __sdcccall(1), a lone uint8_t column arrives in A with zero stack args.\n * Loop-stable pointers are written once by the caller; only per-column geometry\n * is updated before entering the raster kernel. */''','''/* Persistent hot-path context. ABI experiment: the raster kernel receives\n * (uint8_t col, TSRasterCtx *ctx). Under Z80 __sdcccall(1), col is eligible for\n * A and the following 16-bit pointer for DE, so neither argument needs stack\n * transport. Gearsystem profiling and generated assembly decide whether the\n * extra pointer beats direct static/global addressing. */''')
p.write_text(s)
print('Materialized col,ctx register-ABI raster experiment.')
