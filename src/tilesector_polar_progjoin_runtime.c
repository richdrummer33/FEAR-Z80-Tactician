#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar_progjoin_runtime.h"
#include "pj_assets.h"

#define PJ_BANK_BYTES 16384u
#ifndef TSP_PROGJOIN_BANK_BASE
#define TSP_PROGJOIN_BANK_BASE 8u
#endif
#define PJ_BANK_META   (TSP_PROGJOIN_BANK_BASE + 0u)
#define PJ_BANK_DESC   (TSP_PROGJOIN_BANK_BASE + 1u)
#define PJ_BANK_REC0   (TSP_PROGJOIN_BANK_BASE + 2u)
#define PJ_BANK_REC1   (TSP_PROGJOIN_BANK_BASE + 3u)
#define PJ_BANK_BODY0  (TSP_PROGJOIN_BANK_BASE + 4u)

static uint16_t rd16p(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint8_t record_byte(uint16_t off) {
    if (off < PJ_BANK_BYTES) { SWITCH_ROM2(PJ_BANK_REC0); return gg_pj_records0[off]; }
    SWITCH_ROM2(PJ_BANK_REC1); return gg_pj_records1[(uint16_t)(off - PJ_BANK_BYTES)];
}
static const uint8_t *body_base(uint8_t logical_bank) {
    switch (logical_bank) {
    case 0u: SWITCH_ROM2(PJ_BANK_BODY0 + 0u); return gg_pj_body0;
    case 1u: SWITCH_ROM2(PJ_BANK_BODY0 + 1u); return gg_pj_body1;
    case 2u: SWITCH_ROM2(PJ_BANK_BODY0 + 2u); return gg_pj_body2;
    case 3u: SWITCH_ROM2(PJ_BANK_BODY0 + 3u); return gg_pj_body3;
    case 4u: SWITCH_ROM2(PJ_BANK_BODY0 + 4u); return gg_pj_body4;
    default: return (const uint8_t *)0;
    }
}

uint8_t tsp_progjoin_dispatch_body(int16_t step, uint8_t fam, uint8_t want, int16_t iq, TSPProgjoinBodyRef *out) {
    uint16_t step_index; uint8_t page, local_rank, rank, u, base, fi, key; uint16_t slot, desc_i, rec; int16_t acc; const uint8_t *p;
    if (!out || (fam != 0u && fam != 2u) || want == 0u || want > TSP_PROGJOIN_CHUNK_COLS) return 0u;
    if (step < -2048 || step > 2047) return 0u;
    step_index=(uint16_t)((int16_t)(step+2048)); page=(uint8_t)(step_index>>8);
    SWITCH_ROM2(PJ_BANK_META); local_rank=gg_pj_step_local[step_index]; if(local_rank==0xFFu)return 0u;
    slot=(uint16_t)(rd16p(gg_pj_step_page_base+((uint16_t)page<<1))+local_rank);
    acc=(int16_t)(iq+32); u=(uint8_t)acc&127u; rank=0u; p=gg_pj_thresholds+((uint16_t)slot<<3);
    while(rank<(TSP_PROGJOIN_CHUNK_COLS+1u)&&u>=p[rank])++rank;
    base=(uint8_t)(((int16_t)acc>>7)&7); fi=(fam==0u)?0u:1u;
    desc_i=(uint16_t)((slot*12u+(uint16_t)fi*TSP_PROGJOIN_CHUNK_COLS+(uint16_t)(want-1u))<<1);
    SWITCH_ROM2(PJ_BANK_DESC); rec=rd16p(gg_pj_descriptor+desc_i); if(rec==0xFFFFu)return 0u;
    key=(uint8_t)((base<<3)|rank);
    for(;;){uint8_t got=record_byte(rec); if(got==0xFFu)return 0u; if(got==key){out->bank=record_byte((uint16_t)(rec+1u)); out->off=(uint16_t)record_byte((uint16_t)(rec+2u))|((uint16_t)record_byte((uint16_t)(rec+3u))<<8); return 1u;} rec=(uint16_t)(rec+4u);}
}

uint8_t tsp_progjoin_preflight_run(int16_t step,uint8_t fam,uint8_t ncol,int16_t iq0,TSPProgjoinRunPlan *out){
    uint8_t left,i; int16_t iq; if(!out||ncol==0u||ncol>TSP_PROGJOIN_MAX_RUN_COLS)return 0u; out->count=0u;left=ncol;iq=iq0;i=0u;
    while(left){uint8_t want=(left>TSP_PROGJOIN_CHUNK_COLS)?TSP_PROGJOIN_CHUNK_COLS:left; if(i>=TSP_PROGJOIN_MAX_CHUNKS)return 0u; if(!tsp_progjoin_dispatch_body(step,fam,want,iq,&out->bodies[i])){out->count=0u;return 0u;} out->wants[i]=want;iq=(int16_t)(iq+(int16_t)((int16_t)want*step));left=(uint8_t)(left-want);++i;} out->count=i;return 1u;
}
uint8_t tsp_progjoin_play_body(TSPProgjoinBodyRef body,uint8_t *dst,uint16_t dst_bytes,int16_t *cursor){
    const uint8_t *basep=body_base(body.bank),*p;uint8_t n,i;if(!basep||!dst||!cursor||body.off>=PJ_BANK_BYTES||dst_bytes<2u)return 0u;p=basep+body.off;n=*p++;
    for(i=0u;i!=n;++i){uint16_t word;int16_t delta,c=*cursor;if(c<0||c>=(int16_t)(dst_bytes-1u))return 0u;word=(uint16_t)p[0]|((uint16_t)p[1]<<8);delta=(int16_t)((uint16_t)p[2]|((uint16_t)p[3]<<8));p+=4;dst[(uint16_t)c]=(uint8_t)word;dst[(uint16_t)c+1u]=(uint8_t)(word>>8);*cursor=(int16_t)(c+1+delta);}return 1u;
}
uint8_t tsp_progjoin_play_plan(const TSPProgjoinRunPlan *plan,uint8_t *dst,uint16_t dst_bytes,int16_t *cursor){uint8_t i;if(!plan||plan->count==0u||plan->count>TSP_PROGJOIN_MAX_CHUNKS)return 0u;for(i=0u;i!=plan->count;++i)if(!tsp_progjoin_play_body(plan->bodies[i],dst,dst_bytes,cursor))return 0u;return 1u;}

static uint8_t gate_advance(int16_t step_bytes,int8_t *row,uint8_t *col,uint8_t *rowbit,int8_t *cov,int16_t *cursor){
    uint8_t rb=*rowbit;*cursor=(int16_t)(*cursor+step_bytes);if(step_bytes==40){*row=(int8_t)(*row+1);if(rb==7u){*rowbit=0u;*cov=(int8_t)(*cov+1);}else *rowbit=(uint8_t)(rb+1u);return 1u;}*col=(uint8_t)(*col+1u);
    if(step_bytes==2){*cov=(int8_t)(*cov+3);return 1u;}if(step_bytes==-38){*row=(int8_t)(*row-1);*cov=(int8_t)(*cov+((rb==0u)?2:3));*rowbit=(uint8_t)((rb+7u)&7u);return 1u;}if(step_bytes==-78){*row=(int8_t)(*row-2);*cov=(int8_t)(*cov+((rb<2u)?2:3));*rowbit=(uint8_t)((rb+6u)&7u);return 1u;}if(step_bytes==-118){*row=(int8_t)(*row-3);*cov=(int8_t)(*cov+((rb<3u)?2:3));*rowbit=(uint8_t)((rb+5u)&7u);return 1u;}return 0u;
}
uint8_t tsp_progjoin_play_plan_gated(const TSPProgjoinRunPlan *plan,uint16_t *dst_words,const uint8_t *coverage60,uint8_t *row_min18,uint8_t *row_max18,int8_t first_row,uint8_t first_col){
    uint8_t pi;int8_t row,cov;uint8_t col,rowbit;int16_t cursor;if(!plan||!dst_words||!coverage60||!row_min18||!row_max18||plan->count==0u||plan->count>TSP_PROGJOIN_MAX_CHUNKS||first_col>=20u)return 0u;
    row=first_row;col=first_col;rowbit=(uint8_t)first_row&7u;cov=(int8_t)((int8_t)(first_col+first_col+first_col)+((first_row>=0)?(first_row>>3):-1));cursor=(int16_t)((int16_t)first_row*40+(int16_t)first_col*2);
    for(pi=0u;pi!=plan->count;++pi){const TSPProgjoinBodyRef body=plan->bodies[pi];const uint8_t *basep=body_base(body.bank),*p;uint8_t n,i;if(!basep||body.off>=PJ_BANK_BYTES)return 0u;p=basep+body.off;n=*p++;
        for(i=0u;i!=n;++i){uint16_t word=(uint16_t)p[0]|((uint16_t)p[1]<<8);int16_t delta=(int16_t)((uint16_t)p[2]|((uint16_t)p[3]<<8));int16_t step_bytes=(int16_t)(1+delta);p+=4;
            if(row>=0&&row<18&&col<20u){uint8_t mask=(uint8_t)(1u<<rowbit),ci=(uint8_t)cov;if((coverage60[ci]&mask)==0u){uint16_t wi=(uint16_t)((uint16_t)row*20u+col);if(dst_words[wi]!=word){dst_words[wi]=word;if(row_min18[(uint8_t)row]==0xFFu||col<row_min18[(uint8_t)row])row_min18[(uint8_t)row]=col;if(col>row_max18[(uint8_t)row])row_max18[(uint8_t)row]=col;}}}
            if(!gate_advance(step_bytes,&row,&col,&rowbit,&cov,&cursor))return 0u;}}
    return 1u;
}

#if defined(TSPF_PROGJOIN_FULL) && TSPF_PROGJOIN_FULL
extern uint8_t g_polar_nt_cov_cur[60];
extern uint8_t g_polar_nt_row_min[18];
extern uint8_t g_polar_nt_row_max[18];
static int8_t pj_row_floor(int16_t y){return (y>=0)?(int8_t)(y>>3):(int8_t)-(((-y)+7)>>3);}
uint8_t tsp_progjoin_try_full_edges(uint16_t *out,uint8_t c0,uint8_t n,int16_t iq,int16_t step){
    TSPProgjoinRunPlan top,bot;int16_t a0,an,q0,qn,tl,tr,bl,br;uint8_t invl,invr,hl,hr;int8_t top_row,bot_row;
    a0=(int16_t)(iq+32);an=(int16_t)(iq+(int16_t)((int16_t)n*step)+32);q0=(int16_t)(a0>>6);qn=(int16_t)(an>>6);if(q0<0||q0>255||qn<0||qn>255)return 0u;
    if(!tsp_progjoin_preflight_run(step,0u,n,iq,&top))return 0u;if(!tsp_progjoin_preflight_run(step,2u,n,iq,&bot))return 0u;
    invl=(uint8_t)((iq+32)>>6);invr=(uint8_t)((iq+step+32)>>6);hl=(uint8_t)(invl>>1);hr=(uint8_t)(invr>>1);tl=(int16_t)(71-hl);tr=(int16_t)(71-hr);bl=(int16_t)(72+hl);br=(int16_t)(72+hr);
    top_row=pj_row_floor(tl<tr?tl:tr);bot_row=pj_row_floor(bl<br?bl:br);
    if(!tsp_progjoin_play_plan_gated(&top,out,g_polar_nt_cov_cur,g_polar_nt_row_min,g_polar_nt_row_max,top_row,c0))return 0u;
    if(!tsp_progjoin_play_plan_gated(&bot,out,g_polar_nt_cov_cur,g_polar_nt_row_min,g_polar_nt_row_max,bot_row,c0))return 0u;return 1u;
}
#endif
