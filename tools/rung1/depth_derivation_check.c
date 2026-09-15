/* Does the mode-0 ROM derive (iq, step) the same way the corpus was baked?
 *
 * The renderer has two derivations, chosen by r->depth_plane:
 *
 *   mode<2 && depth_plane : iq = r->iq,  step = r->step      <- screen_depth_plane
 *   otherwise             : iq = inv0<<6, step = ((inv1-inv0)*recip)>>2
 *
 * screen_depth_plane is guarded `#if defined(__SDCC) && TSPF_SCREEN_DEPTH_PLANE`
 * at all eight of its sites, so it does not exist in a host build. The pose
 * oracle, and therefore the whole compiled corpus, is produced by a host build
 * and can only ever use the second derivation. The shipped mode-0 ROM uses the
 * first. This transcribes screen_depth_plane onto the host, using the generated
 * (class, yaw) coefficient tables and the loader's exact indexing
 * (nf[c] = k_depth_nf_q7[c][yaw]), and compares the two over arbitrary poses:
 * every walkable cell, six sub-cell offsets, all 256 headings.
 *
 * dp_tables.h is extracted verbatim from the generated depth-plane bake. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static int16_t dp_shr(int16_t v,uint8_t n){ return shr_signed(v,n); }

/* screen_depth_plane, transcribed from the renderer's SDCC-only path */
static uint8_t dp_derive(uint8_t sid,uint8_t invd,uint8_t c0,uint8_t c1,uint8_t yaw,
                         int16_t *out_iq,int16_t *out_step)
{
    uint8_t cls=k_dp_normal_class[sid];
    int8_t nf=k_depth_nf_q7[cls][yaw], sf=k_depth_stepfac_q4[cls][yaw];
    int16_t iq=dp_shr((int16_t)((int16_t)invd*(int16_t)nf),1);
    int16_t step=dp_shr((int16_t)((int16_t)invd*(int16_t)sf),4);
    int16_t endq; uint8_t i,n=(uint8_t)(c1-c0+1u);
    if(c0<10u){ for(i=c0;i<10u;++i) iq=(int16_t)(iq-step); }
    else      { for(i=10u;i<c0;++i) iq=(int16_t)(iq+step); }
    endq=iq;
    for(i=0u;i<n;++i) endq=(int16_t)(endq+step);
    if((iq<0&&endq>0)||(iq>0&&endq<0)) return 0u;
    if(iq<0||endq<0){ iq=(int16_t)-iq; endq=(int16_t)-endq; step=(int16_t)-step; }
    *out_iq=iq; *out_step=step; return 1u;
}

int main(void)
{
    TSPState s; unsigned gx,gy,yaw,oi,i,c;
    static const int8_t off[][2]={{0,0},{7,3},{3,7},{11,5},{19,23},{41,37}};
    unsigned long runs=0,dp_applies=0,step_same=0,iq_same=0,both_same=0;
    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t px0=(int16_t)(gx*CELL_Q4+32),py0=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(px0,py0)) continue;
        for(oi=0;oi<6u;++oi){
            int16_t px=(int16_t)(px0+off[oi][0]),py=(int16_t)(py0+off[oi][1]);
            if(!tsp_is_walkable_q4(px,py)) continue;
            for(yaw=0;yaw<256u;yaw+=1u){
                uint8_t ks[64],nk=0,count=0,j;
                uint8_t recipe,base_id,cond_count,lx,ly; uint16_t gi,offs;
                const uint8_t *p,*b;
                memset(&s,0,sizeof s); s.x_q4=px; s.y_q4=py; s.yaw=(uint8_t)yaw;
                gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);
                recipe=k_tspf_recipe_grid[gi]; if(recipe==0xffu) continue;
                lx=(uint8_t)((uint16_t)px&63u); ly=(uint8_t)((uint16_t)py&63u);
                offs=k_tspf_recipe_off[recipe]; p=&k_tspf_recipe_stream[offs];
                base_id=*p++; cond_count=*p++;
                b=&k_tspf_base_stream[k_tspf_base_off[base_id]]; i=*b++;
                for(;i;--i) ks[nk++]=*b++;
                for(i=0;i<cond_count;++i){ uint8_t key=*p++,sel=*p++; if(selector_pass(sel,lx,ly)) ks[nk++]=key; }
                tsp_polar_renderer_reset(); g_corner_bearing_valid=0u;
                for(j=0;j<nk;++j){ if(count>=TSPF_MAX_ACTIVE) break;
                    if(!project_key(ks[j],&s,&g_runs[count])) continue; insert_run(count,&count); }
                for(c=0;c<count;++c){
                    PolarRun *r=&g_runs[c];
                    uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),n;
                    int16_t iq_h,step_h,iq_d,step_d; uint8_t invd;
                    if(cc0>=TSP_COLS) cc0=TSP_COLS-1; if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
                    if(cc1<cc0) continue;
                    if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
                    n=(uint8_t)(cc1-cc0+1u);
                    iq_h=(int16_t)((int16_t)r->inv0<<6);
                    step_h=dp_shr((int16_t)(((int16_t)r->inv1-(int16_t)r->inv0)*(int16_t)k_col_recip_q8[n]),2);
                    invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],&s));
                    ++runs;
                    if(!dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&iq_d,&step_d)) continue;
                    ++dp_applies;
                    if(step_h==step_d) ++step_same;
                    if(iq_h==iq_d) ++iq_same;
                    if(step_h==step_d&&iq_h==iq_d) ++both_same;
                }
            }
        }
    }
    printf("FULL runs examined            %lu\n",runs);
    printf("  depth-plane path applies    %lu (%.1f%%)\n",dp_applies,100.0*dp_applies/(double)runs);
    printf("  of those, step matches host %lu (%.2f%%)\n",step_same,dp_applies?100.0*step_same/(double)dp_applies:0.0);
    printf("  of those, iq   matches host %lu (%.2f%%)\n",iq_same,dp_applies?100.0*iq_same/(double)dp_applies:0.0);
    printf("  of those, BOTH match        %lu (%.2f%%)\n",both_same,dp_applies?100.0*both_same/(double)dp_applies:0.0);
    return 0;
}
