#!/usr/bin/env python3
from pathlib import Path

p=Path('src/tilesector_core.c')
s=p.read_text()
if 'uint8_t inv_mid;' in s:
    print('pixel-profile hot-path simplification already materialized')
    raise SystemExit(0)

def repl(old,new,label):
    global s
    if old not in s:
        raise SystemExit(f'pattern not found: {label}')
    s=s.replace(old,new,1)

# Raster only needs midpoint reciprocal for fog/diagnostics; don't shuttle two
# Q6 endpoints through the persistent context merely to average them again.
repl('''    uint8_t seg_id;\n    int16_t inv_l_q6, inv_r_q6;\n    int16_t top_l, top_r, bot_l, bot_r;\n''','''    uint8_t seg_id;\n    uint8_t inv_mid;\n    int16_t top_l, top_r, bot_l, bot_r;\n''','ctx reciprocal midpoint')
repl('''    TS_FAST_LOCAL uint8_t seg_id;\n    TS_FAST_LOCAL int16_t inv_l_q6,inv_r_q6;\n    TS_FAST_LOCAL int16_t top_l,top_r,bot_l,bot_r;\n''','''    TS_FAST_LOCAL uint8_t seg_id,inv_mid;\n    TS_FAST_LOCAL int16_t top_l,top_r,bot_l,bot_r;\n''','raster reciprocal local')
repl('''    seg_id=g_raster_ctx.seg_id;\n    inv_l_q6=g_raster_ctx.inv_l_q6; inv_r_q6=g_raster_ctx.inv_r_q6;\n    top_l=g_raster_ctx.top_l; top_r=g_raster_ctx.top_r;\n''','''    seg_id=g_raster_ctx.seg_id;\n    inv_mid=g_raster_ctx.inv_mid;\n    top_l=g_raster_ctx.top_l; top_r=g_raster_ctx.top_r;\n''','raster reciprocal load')
repl('''    seg=&k_segments[seg_id];\n    shade=shade_for((uint8_t)(((inv_l_q6+inv_r_q6)>>1)>>6),seg->shade_bias);\n''','''    seg=&k_segments[seg_id];\n    shade=shade_for(inv_mid,seg->shade_bias);\n''','shade midpoint')
repl('''        uint8_t inv = (uint8_t)(((inv_l_q6 + inv_r_q6) >> 1) >> 6);\n        if (inv > cols[col].invz) {\n            cols[col].invz = inv;\n''','''        uint8_t inv = inv_mid;\n        if (inv > cols[col].invz) {\n            cols[col].invz = inv;\n''','host midpoint')

# Solid sectors only contain FULL or RAISED_FULL profiles. Derive exact rounded
# pixel endpoints directly from reciprocal half-extent; avoid profile_y_q6's
# pointer outputs plus four generic signed q6_round calls per column.
old='''        int16_t tlq,trq,blq,brq;\n        int16_t top_l,top_target,top_r,bot_l,bot_target,bot_r;\n        uint8_t stl,sbl,str,sbr;\n        if (seg_id==TS_NO_WALL || g_clip_top[depth][c]>g_clip_bottom[depth][c]) {\n            prev_seg=TS_NO_WALL;\n            continue;\n        }\n        profile_y_q6(k_segments[seg_id].profile,g_best_inv_l_q6[c],&tlq,&blq,&stl,&sbl);\n        profile_y_q6(k_segments[seg_id].profile,g_best_inv_r_q6[c],&trq,&brq,&str,&sbr);\n        if (stl||str) {\n            top_l=(int16_t)((q6_round_px(tlq)+4)&~7);\n            top_target=(int16_t)((q6_round_px(trq)+4)&~7);\n        } else { top_l=q6_round_px(tlq); top_target=q6_round_px(trq); }\n        if (sbl||sbr) {\n            bot_l=(int16_t)(((q6_round_px(blq)+4)&~7)-1);\n            bot_target=(int16_t)(((q6_round_px(brq)+4)&~7)-1);\n        } else { bot_l=q6_round_px(blq); bot_target=q6_round_px(brq); }\n'''
new='''        int16_t top_l,top_target,top_r,bot_l,bot_target,bot_r;\n        int16_t hl,hr;\n        uint8_t profile;\n        if (seg_id==TS_NO_WALL || g_clip_top[depth][c]>g_clip_bottom[depth][c]) {\n            prev_seg=TS_NO_WALL;\n            continue;\n        }\n        profile=k_segments[seg_id].profile;\n        hl=(int16_t)(g_best_inv_l_q6[c]>>1);\n        hr=(int16_t)(g_best_inv_r_q6[c]>>1);\n        top_l=(int16_t)(TS_HORIZON-((hl+31)>>6));\n        top_target=(int16_t)(TS_HORIZON-((hr+31)>>6));\n        if (profile==TS_PROFILE_RAISED_FULL) {\n            hl=(int16_t)(hl-(hl>>2));\n            hr=(int16_t)(hr-(hr>>2));\n        }\n        bot_l=(int16_t)(TS_HORIZON+((hl+32)>>6));\n        bot_target=(int16_t)(TS_HORIZON+((hr+32)>>6));\n'''
repl(old,new,'solid profile pixel math')
repl('''        g_raster_ctx.seg_id=seg_id;\n        g_raster_ctx.inv_l_q6=g_best_inv_l_q6[c];\n        g_raster_ctx.inv_r_q6=g_best_inv_r_q6[c];\n        g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;\n        g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;\n        g_raster_ctx.snap_top=(uint8_t)(stl||str);\n        g_raster_ctx.snap_bottom=(uint8_t)(sbl||sbr);\n''','''        g_raster_ctx.seg_id=seg_id;\n        g_raster_ctx.inv_mid=g_best_inv[c];\n        g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;\n        g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;\n        g_raster_ctx.snap_top=0u;\n        g_raster_ctx.snap_bottom=0u;\n''','solid ctx midpoint')

# Portal faces are only LINTEL or RISER. Same exact pixel rounding, followed by
# the authored tile-row snap on the opening boundary.
old='''        int16_t next_q6=(int16_t)(inv_q6+p->step_q6);\n        int16_t tlq,trq,blq,brq;\n        int16_t top_l,top_target,top_r,bot_l,bot_target,bot_r;\n        uint8_t stl,sbl,str,sbr,border=0u;\n        uint8_t uc=(uint8_t)c;\n        if (g_clip_top[depth][uc]<=g_clip_bottom[depth][uc]) {\n            profile_y_q6(k_segments[seg_id].profile,inv_q6,&tlq,&blq,&stl,&sbl);\n            profile_y_q6(k_segments[seg_id].profile,next_q6,&trq,&brq,&str,&sbr);\n            if (stl||str) {\n                top_l=(int16_t)((q6_round_px(tlq)+4)&~7);\n                top_target=(int16_t)((q6_round_px(trq)+4)&~7);\n            } else { top_l=q6_round_px(tlq); top_target=q6_round_px(trq); }\n            if (sbl||sbr) {\n                bot_l=(int16_t)(((q6_round_px(blq)+4)&~7)-1);\n                bot_target=(int16_t)(((q6_round_px(brq)+4)&~7)-1);\n            } else { bot_l=q6_round_px(blq); bot_target=q6_round_px(brq); }\n'''
new='''        int16_t next_q6=(int16_t)(inv_q6+p->step_q6);\n        int16_t top_l,top_target,top_r,bot_l,bot_target,bot_r;\n        int16_t hl,hr;\n        uint8_t snap_top=0u,snap_bottom=0u,border=0u;\n        uint8_t uc=(uint8_t)c;\n        if (g_clip_top[depth][uc]<=g_clip_bottom[depth][uc]) {\n            hl=(int16_t)(inv_q6>>1);\n            hr=(int16_t)(next_q6>>1);\n            if (k_segments[seg_id].profile==TS_PROFILE_RISER) {\n                hl=(int16_t)(hl-(hl>>2));\n                hr=(int16_t)(hr-(hr>>2));\n                top_l=(int16_t)((TS_HORIZON+((hl+32)>>6)+4)&~7);\n                top_target=(int16_t)((TS_HORIZON+((hr+32)>>6)+4)&~7);\n                /* bottom still uses the original half extent, not raised 3/4. */\n                hl=(int16_t)(inv_q6>>1); hr=(int16_t)(next_q6>>1);\n                bot_l=(int16_t)(TS_HORIZON+((hl+32)>>6));\n                bot_target=(int16_t)(TS_HORIZON+((hr+32)>>6));\n                snap_top=1u;\n            } else {\n                top_l=(int16_t)(TS_HORIZON-((hl+31)>>6));\n                top_target=(int16_t)(TS_HORIZON-((hr+31)>>6));\n                hl=(int16_t)(hl>>1); hr=(int16_t)(hr>>1);\n                bot_l=(int16_t)(((TS_HORIZON-((hl+31)>>6)+4)&~7)-1);\n                bot_target=(int16_t)(((TS_HORIZON-((hr+31)>>6)+4)&~7)-1);\n                snap_bottom=1u;\n            }\n'''
repl(old,new,'portal profile pixel math')
repl('''            g_raster_ctx.seg_id=seg_id;\n            g_raster_ctx.inv_l_q6=inv_q6; g_raster_ctx.inv_r_q6=next_q6;\n            g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;\n            g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;\n            g_raster_ctx.snap_top=(uint8_t)(stl||str);\n            g_raster_ctx.snap_bottom=(uint8_t)(sbl||sbr);\n''','''            g_raster_ctx.seg_id=seg_id;\n            g_raster_ctx.inv_mid=(uint8_t)(((inv_q6+next_q6)>>1)>>6);\n            g_raster_ctx.top_l=top_l; g_raster_ctx.top_r=top_r;\n            g_raster_ctx.bot_l=bot_l; g_raster_ctx.bot_r=bot_r;\n            g_raster_ctx.snap_top=snap_top;\n            g_raster_ctx.snap_bottom=snap_bottom;\n''','portal ctx midpoint')

p.write_text(s)
print('Materialized exact pixel-profile + reciprocal-midpoint hot path.')
