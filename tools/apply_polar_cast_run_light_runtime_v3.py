#!/usr/bin/env python3
"""Third-stage Polar cast-light runtime patch.

Keep the v2 compact cast-plane / per-column interval math unchanged, but move
accepted name-table word stores and solid lit spans onto a dedicated Z80
materializer.  Degenerate columns still use the C reference path.
"""
from pathlib import Path
import re
import runpy

runpy.run_path("tools/apply_polar_cast_run_light_runtime_v2.py", run_name="__main__")

p=Path("src/tilesector_polar_cast_light_runtime.c")
s=p.read_text()

anchor="static uint8_t g_cast_yaw_cache=0xffu;"
bridge='''static uint8_t g_cast_yaw_cache=0xffu;

/* Explicit fixed-assembly bridge for the hardware-shaped cast materializer. */
uint8_t g_cast_mat_col;
uint8_t g_cast_mat_row;
uint8_t g_cast_mat_first;
uint8_t g_cast_mat_last;
uint16_t g_cast_mat_word;
void tsp_polar_cast_store_word_fast(void);
void tsp_polar_cast_fill_fast(void);'''
if s.count(anchor)!=1: raise SystemExit("v3 bridge anchor missing")
s=s.replace(anchor,bridge,1)

old_boundary=re.compile(r'''static void cast_draw_boundary\(uint16_t \*out,uint8_t col,int16_t yl,int16_t yr,uint8_t lit_below\)\{.*?\n\}\n\nstatic void cast_fill_rows\(uint16_t \*out,uint8_t col,int8_t first,int8_t last\)\{.*?\n\}''',re.S)
m=old_boundary.search(s)
if not m: raise SystemExit("v3 boundary/fill block missing")
new=r'''static void cast_draw_boundary(uint16_t *out,uint8_t col,int16_t yl,int16_t yr,uint8_t lit_below){
    int16_t lo=yl<yr?yl:yr,hi=yl>yr?yl:yr;int8_t r0,r1,r;int8_t slope;
    (void)out;
    if(hi<((int16_t)CAST_FLOOR_FIRST_ROW<<3)||lo>=144)return;
    if(lo<0)lo=0;if(hi>143)hi=143;
    r0=(int8_t)(lo>>3);r1=(int8_t)(hi>>3);
    if(r0<(int8_t)CAST_FLOOR_FIRST_ROW)r0=(int8_t)CAST_FLOOR_FIRST_ROW;
    if(r1>=(int8_t)TSP_ROWS)r1=(int8_t)(TSP_ROWS-1u);
    slope=cast_clamp_s8((int16_t)(yr-yl),-7,7);
    g_cast_mat_col=col;
    for(r=r0;r<=r1;++r){
        g_cast_mat_row=(uint8_t)r;
        g_cast_mat_word=cast_edge_word((int16_t)(yl-((int16_t)r<<3)),slope,lit_below);
        tsp_polar_cast_store_word_fast();
    }
}

static void cast_fill_rows(uint16_t *out,uint8_t col,int8_t first,int8_t last){
    (void)out;
    if(first<(int8_t)CAST_FLOOR_FIRST_ROW)first=(int8_t)CAST_FLOOR_FIRST_ROW;
    if(last>=(int8_t)TSP_ROWS)last=(int8_t)(TSP_ROWS-1u);
    if(first>last)return;
    g_cast_mat_col=col;g_cast_mat_first=(uint8_t)first;g_cast_mat_last=(uint8_t)last;
    g_cast_mat_word=CAST_LIT_FLOOR_WORD;
    tsp_polar_cast_fill_fast();
}'''
s=s[:m.start()]+new+s[m.end():]
p.write_text(s)

mk=Path("Makefile");ms=mk.read_text()
obj="build/tilesector_polar_cast_light_runtime_gg.o build/tilesector_polar_castplane_gg.o"
if ms.count(obj)!=1: raise SystemExit("v3 Makefile object anchor missing")
ms=ms.replace(obj,obj+" build/tilesector_polar_cast_materialize_v3_gg.o",1)
rule='''build/tilesector_polar_cast_materialize_v3_gg.o: src/tilesector_polar_cast_materialize_v3_gg.s | build
\t$(LCC) $(POLAR_GGFLAGS) -c -o $@ $<

'''
anchor_rule="build/tilesector_polar_castplane_gg.o: build/generated/polar_cast_plane/tilesector_polar_castplane.c build/generated/polar_cast_plane/tilesector_polar_cast_plane_meta.h | build\n\t$(LCC) $(POLAR_GGFLAGS) $(POLAR_CFLAGS) -c -o $@ build/generated/polar_cast_plane/tilesector_polar_castplane.c\n\n"
if ms.count(anchor_rule)!=1: raise SystemExit("v3 Makefile rule anchor missing")
ms=ms.replace(anchor_rule,anchor_rule+rule,1);mk.write_text(ms)

print("POLAR_CAST_RUN_V3=Z80_NAME_TABLE_MATERIALIZER")
print("CAST_NORMAL_FULL_SPAN_C_LOOPS=0")
print("CAST_NORMAL_CELL_OWNERSHIP_TESTS=ASSEMBLY")
print("CAST_NORMAL_DIRTY_TRACKING=ASSEMBLY")
print("CAST_GEOMETRY_MATH=UNCHANGED_FROM_V2")
print("CAST_DEGENERATE_FALLBACK=REFERENCE_C")
