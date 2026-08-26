#!/usr/bin/env python3
from pathlib import Path
import re

p=Path('src/tilesector_core.c')
s=p.read_text()
if 'g_candidate_ctx' in s:
    print('Stage 11 candidate assembly bridge already applied')
    raise SystemExit(0)

# Assembly needs direct names for the five hot candidate arrays. The winner
# IDs/depth/border are bytes; interpolated reciprocal endpoints are signed Q6.
types={
    'g_best_seg':'uint8_t',
    'g_best_inv':'uint8_t',
    'g_best_border':'uint8_t',
    'g_best_inv_l_q6':'int16_t',
    'g_best_inv_r_q6':'int16_t',
}
for name,ctype in types.items():
    s2=re.sub(rf'^static ({ctype} {name}\[TS_COLS\];)',r'\1',s,flags=re.M)
    if s2==s:
        raise SystemExit(f'candidate array declaration not found: {name}')
    s=s2

anchor='''typedef struct {
    int8_t c0, c1;
    int16_t inv_q6;
    int16_t step_q6;
    int8_t original_c0, original_c1;
} TSProjectedSpan;
'''
if anchor not in s:
    raise SystemExit('TSProjectedSpan anchor not found')
bridge=anchor+r'''

/* Stage 11 GG candidate ABI. The hand-written kernel consumes only this
 * five-byte context; host/reference builds keep the C loop below. */
typedef struct {
    uint8_t seg_id;
    uint8_t view_c0;
    uint8_t view_c1;
    const TSProjectedSpan *span;
} TSCandidateCtx;
TSCandidateCtx g_candidate_ctx;
#ifdef __SDCC
void ts_candidate_span_fast(void);
#endif
'''
s=s.replace(anchor,bridge,1)

m=re.search(r'static void candidate_add_segment\(uint8_t seg_id,uint8_t view_c0,uint8_t view_c1\) \{.*?\n\}\n\nstatic void build_sector_candidates',s,re.S)
if not m:
    raise SystemExit('candidate_add_segment function not found')
new=r'''static void candidate_add_segment(uint8_t seg_id,uint8_t view_c0,uint8_t view_c1) {
    const TSProjectedSpan *p=project_segment_span(seg_id);
    if (!p) return;
#ifdef __SDCC
    g_candidate_ctx.seg_id=seg_id;
    g_candidate_ctx.view_c0=view_c0;
    g_candidate_ctx.view_c1=view_c1;
    g_candidate_ctx.span=p;
    ts_candidate_span_fast();
#else
    {
        int8_t c,c0=p->c0,c1=p->c1;
        int16_t inv_q6=p->inv_q6;
        while (c0<(int8_t)view_c0) { inv_q6=(int16_t)(inv_q6+p->step_q6); ++c0; }
        if (c1>(int8_t)view_c1) c1=(int8_t)view_c1;
        if (c0>c1) return;
        for (c=c0;c<=c1;++c) {
            uint8_t uc=(uint8_t)c;
            int16_t next_q6=(int16_t)(inv_q6+p->step_q6);
            uint8_t inv=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
            if (g_best_seg[uc]==TS_NO_WALL || inv>g_best_inv[uc]) {
                uint8_t border=0u;
                if (c==p->original_c0 && p->original_c0>=0) border|=1u;
                if (c==p->original_c1 && p->original_c1<(int8_t)TS_COLS) border|=2u;
                g_best_seg[uc]=seg_id;
                g_best_inv[uc]=inv;
                g_best_border[uc]=border;
                g_best_inv_l_q6[uc]=inv_q6;
                g_best_inv_r_q6[uc]=next_q6;
            }
            inv_q6=next_q6;
        }
    }
#endif
}

static void build_sector_candidates'''
s=s[:m.start()]+new+s[m.end():]
p.write_text(s)
print('Applied Stage 11 hand-written candidate-span bridge.')
