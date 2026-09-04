#!/usr/bin/env python3
"""Apply the experimental zero-new-pattern floor-light runtime integration.

This deliberately patches only the CI/worktree used for the runtime probe.  The
branch's known-good Polar renderer remains a clean baseline while the workflow
can build baseline and lit ROMs from the same commit and cycle-profile the delta.
"""
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    n = text.count(old)
    if n != 1:
        raise SystemExit(f"{label}: expected exactly one anchor, found {n}")
    return text.replace(old, new, 1)


renderer = Path("src/tilesector_polar_renderer.c")
s = renderer.read_text()

include_anchor = "\nvoid tsp_polar_render(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS],TSPColumn cols[TSP_COLS]) BANKED {"
include_repl = "\n#include \"tilesector_polar_floor_light.inc\"\n\nvoid tsp_polar_render(const TSPState *s,uint16_t out_map[TSP_MAP_CELLS],TSPColumn cols[TSP_COLS]) BANKED {"
s = replace_once(s, include_anchor, include_repl, "renderer include")

call_anchor = "    TSPF_SET_STAGE(4u);for(i=0;i<count;++i)draw_run(out_map,cols,&g_runs[g_run_order[i]]);\ndone:"
call_repl = "    TSPF_SET_STAGE(4u);for(i=0;i<count;++i)draw_run(out_map,cols,&g_runs[g_run_order[i]]);\n#ifdef __SDCC\n    if(g_tspf_appearance_mode==0u)tsp_polar_floor_light(s,out_map);\n#endif\ndone:"
s = replace_once(s, call_anchor, call_repl, "renderer light call")
renderer.write_text(s)

main = Path("src/main_tilesector_polar_gg.c")
m = main.read_text()

# Palette 1 index 1 remains the normal lit floor colour.  Index 2 becomes the
# shadow floor colour.  The ordinary floor pattern is solid index 2, so adding
# the palette attribute creates a full shadow tile with zero new patterns.
pal_anchor = "    RGB(0,0,0),RGB(2,2,3),RGB(2,2,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),"
pal_repl = "    RGB(0,0,0),RGB(2,2,3),RGB(1,1,3),RGB(3,4,6),RGB(6,7,9),RGB(10,11,13),"
m = replace_once(m, pal_anchor, pal_repl, "palette-1 shadow slot")

edge_anchor = "static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){uint8_t x,y,color=shade_color(shade);int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x){int8_t line=(int8_t)(off+k_edge_lut[si][x]);uint8_t c=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);paint_pixel(x,y,c);}set_bkg_4bpp_data(TSP_TILE_EDGE(shade,oi,si),1u,g_tile);}"
edge_repl = "static void emit_edge(uint8_t shade,uint8_t oi,uint8_t si){uint8_t x,y,color=shade_color(shade);int8_t off=(int8_t)TSP_EDGE_OFF_MIN+(int8_t)oi;clear_tile();for(y=0;y<8u;++y)for(x=0;x<8u;++x){int8_t line=(int8_t)(off+k_edge_lut[si][x]);uint8_t c;if(shade==0u)c=(int8_t)y<line?C_OUT:C_FLOOR;else c=(int8_t)y<line?C_OUT:((int8_t)y==line?C_BLACK:color);paint_pixel(x,y,c);}set_bkg_4bpp_data(TSP_TILE_EDGE(shade,oi,si),1u,g_tile);}"
m = replace_once(m, edge_anchor, edge_repl, "shade-zero floor edge authoring")

# This experiment intentionally owns appearance mode zero.  Disable the START
# appearance-mode cycle so shade-zero patterns can never be reinterpreted as
# far-wall patterns while the floor-light semantic pass is active.
toggle_anchor = "    if(pressed&J_START){if(pad&J_B)g_tspf_appearance_mode=0u;else if(pad&J_A)g_tspf_appearance_mode=2u;else {++g_tspf_appearance_mode;if(g_tspf_appearance_mode>2u)g_tspf_appearance_mode=0u;}}"
toggle_repl = "    if(pressed&J_START)g_tspf_appearance_mode=0u;"
m = replace_once(m, toggle_anchor, toggle_repl, "appearance-mode pin")
main.write_text(m)

print("FLOOR_LIGHT_RUNTIME_PATCH=APPLIED")
print("LIGHT_POLYGONS=4")
print("INTERNAL_CAST_EDGES=4")
print("NEW_VRAM_PATTERNS=0")
print("SHADE0_EDGE_FAMILY=REPURPOSED_FOR_FLOOR_LIGHT")
print("PALETTE1_FLOOR=FULL_SHADOW_TILE")
