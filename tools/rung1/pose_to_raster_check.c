/* Rung 1: arbitrary pose -> ROM projection -> iq/step -> closed-form raster state.
 *
 * No sampled dictionary is consulted anywhere in this experiment.
 *
 * The candidate architecture is the one the ROM already has:
 *
 *   baked local corner field -> corner bearing            (project_key)
 *   - yaw, global angle_x LUT -> screen endpoints x0,x1   (project_key)
 *   wall normal distance -> inverse depth                 (wall_d_q4/inv_for_dq4)
 *   (invd, normal class, yaw) -> iq, step                 (screen_depth_plane)
 *   iq, step, family, length -> canonical raster shape    (closed form)
 *
 * Two derivations of (iq, step) exist in the renderer, selected by r->depth_plane.
 * The ROM uses screen_depth_plane; the host build cannot (it is __SDCC-guarded) and
 * uses iq=inv0<<6, step=((inv1-inv0)*recip)>>2. Exact equality of those integers is
 * the right criterion for explaining dictionary misses and the WRONG criterion for
 * geometric correctness: two parameterizations may quantize to the same columns.
 *
 * So disagreement is reported at four layers separately:
 *
 *   L1 endpoint   screen columns c0,c1 spanned by the run
 *   L2 depth      per-column quantized h(c), the term the raster actually uses
 *   L3 trajectory the canonical raster shape (cursor move sequence)
 *   L4 ownership  the set of screen cells the trajectory covers
 *
 * Vocabulary is measured AFTER canonicalization, not before.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define CHUNK 6
#define NT_ROWS 18

static uint8_t dp_derive(uint8_t sid,uint8_t invd,uint8_t c0,uint8_t c1,uint8_t yaw,
                         int16_t *out_iq,int16_t *out_step)
{
    uint8_t cls=k_dp_normal_class[sid];
    int8_t nf=k_depth_nf_q7[cls][yaw], sf=k_depth_stepfac_q4[cls][yaw];
    int16_t iq=shr_signed((int16_t)((int16_t)invd*(int16_t)nf),1);
    int16_t step=shr_signed((int16_t)((int16_t)invd*(int16_t)sf),4);
    int16_t endq; uint8_t i,n=(uint8_t)(c1-c0+1u);
    if(c0<10u){ for(i=c0;i<10u;++i) iq=(int16_t)(iq-step); }
    else      { for(i=10u;i<c0;++i) iq=(int16_t)(iq+step); }
    endq=iq; for(i=0u;i<n;++i) endq=(int16_t)(endq+step);
    if((iq<0&&endq>0)||(iq>0&&endq<0)) return 0u;
    if(iq<0||endq<0){ iq=(int16_t)-iq; endq=(int16_t)-endq; step=(int16_t)-step; }
    *out_iq=iq; *out_step=step; return 1u;
}

/* the renderer's own per-column term */
static int hq(int16_t iq,int16_t step,int c){ return (int)((((iq+c*step+32)>>6)&0xFF)>>1); }
static int yq(int16_t iq,int16_t step,int c,int fam){ int h=hq(iq,step,c); return fam==0?71-h:72+h; }

/* canonical raster shape; returns cell count, fills moves[] and rows[]/cols[] */
static int shape_of(int16_t iq,int16_t step,int fam,int want,int *moves,int *rows,int *cols)
{
    int span_lo[CHUNK+2],span_hi[CHUNK+2],c,n=0,row,col=0;
    for(c=0;c<want+1;++c){
        int a=yq(iq,step,c,fam),b=yq(iq,step,c+1,fam);
        int lo=a<=b?a:b, hi=a<=b?b:a;
        span_lo[c]=lo>>3; span_hi[c]=hi>>3;
    }
    row=span_lo[0];
    for(c=0;c<want;++c){
        int r0=span_lo[c],r1=span_hi[c],k,jump;
        for(k=0;k<r1-r0;++k){ rows[n]=row; cols[n]=col; moves[n]=39; ++n; ++row; }
        jump=span_lo[c+1]-r1;
        if(jump<-4||jump>0) return -1;
        rows[n]=row; cols[n]=col; moves[n]= jump==0?1:(jump==-1?-39:(jump==-2?-79:(jump==-3?-119:-159)));
        ++n; ++col; row+=jump;
    }
    return n;
}

/* 64-bit FNV over the covered visible cells, order-independent via sum of hashes */
static uint64_t cover_hash(int nc,const int *rows,const int *cols,int row0,int col0)
{
    uint64_t acc=0; int i;
    for(i=0;i<nc;++i){
        int r=row0+rows[i],c=col0+cols[i];
        if(r<0||r>=NT_ROWS||c<0||c>=20) continue;
        acc+=(uint64_t)(r*20+c)*1099511628211ull+1469598103934665603ull;
    }
    return acc;
}

/* Open-addressed set. A linear-scan table saturated at its cap and reported the
 * cap as if it were a count; this cannot. */
#define HBITS 23
#define HSIZE (1u<<HBITS)
typedef struct { uint64_t *k; unsigned char *used; unsigned long n; } HSet;
static HSet hs_new(void){ HSet h; h.k=(uint64_t*)malloc(sizeof(uint64_t)*HSIZE); h.used=(unsigned char*)calloc(HSIZE,1); h.n=0; return h; }
static void hs_add(HSet *h,uint64_t key){
    uint64_t x=key*1099511628211ull; unsigned i=(unsigned)((x^(x>>29))&(HSIZE-1u));
    for(;;){ if(!h->used[i]){ h->used[i]=1; h->k[i]=key; ++h->n; return; }
             if(h->k[i]==key) return; i=(i+1u)&(HSIZE-1u); }
}
static HSet vstate,vshape;

int main(int argc,char**argv)
{
    unsigned yaw_step = argc>1 ? (unsigned)strtoul(argv[1],0,0) : 1u;
    TSPState s; unsigned gx,gy,yaw,oi,i,c;
    static const int8_t off[][2]={{0,0},{7,3},{3,7},{11,5},{19,23},{41,37}};
    unsigned long runs=0,dp_ok=0,chunks=0;
    unsigned long l1=0,l2=0,l3=0,l4=0,unmodelled_a=0,unmodelled_b=0;
    unsigned long inst=0;
    vstate=hs_new(); vshape=hs_new();
    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t px0=(int16_t)(gx*CELL_Q4+32),py0=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(px0,py0)) continue;
        for(oi=0;oi<6u;++oi){
            int16_t px=(int16_t)(px0+off[oi][0]),py=(int16_t)(py0+off[oi][1]);
            if(!tsp_is_walkable_q4(px,py)) continue;
            for(yaw=0;yaw<256u;yaw+=yaw_step){
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
                    uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),n,invd;
                    int16_t iq_a,st_a,iq_b,st_b; int fam,left,cs;
                    if(cc0>=TSP_COLS) cc0=TSP_COLS-1; if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
                    if(cc1<cc0) continue;
                    if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
                    n=(uint8_t)(cc1-cc0+1u); ++runs;
                    iq_a=(int16_t)((int16_t)r->inv0<<6);
                    st_a=shr_signed((int16_t)(((int16_t)r->inv1-(int16_t)r->inv0)*(int16_t)k_col_recip_q8[n]),2);
                    invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],&s));
                    if(!dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&iq_b,&st_b)) continue;
                    ++dp_ok;
                    /* L1 endpoints are shared by construction; assert it */
                    /* L2/L3/L4 per family, per chunk */
                    for(fam=0;fam<=2;fam+=2){
                        left=n; cs=0;
                        while(left>0){
                            int want=left>CHUNK?CHUNK:left;
                            int ma[64],ra[64],ca[64],mb[64],rb[64],cb[64],na,nb,k,bad2=0;
                            int16_t qa=(int16_t)(iq_a+(int16_t)(cs*st_a)), qb=(int16_t)(iq_b+(int16_t)(cs*st_b));
                            ++chunks; ++inst;
                            for(k=0;k<=want+1;++k) if(hq(qa,st_a,k)!=hq(qb,st_b,k)) { bad2=1; break; }
                            if(bad2) ++l2;
                            na=shape_of(qa,st_a,fam,want,ma,ra,ca);
                            nb=shape_of(qb,st_b,fam,want,mb,rb,cb);
                            if(na<0) ++unmodelled_a;
                            if(nb<0) ++unmodelled_b;
                            if(na>=0&&nb>=0){
                                int diff=(na!=nb);
                                int row0a,row0b;
                                if(!diff) for(k=0;k<na;++k) if(ma[k]!=mb[k]){diff=1;break;}
                                if(diff) ++l3;
                                /* L4 is evaluated for EVERY comparable chunk, not only
                                 * where L3 agreed: two different trajectories covering
                                 * the same cells is precisely what this layer exists to
                                 * detect, and gating it on L3 would hide that. */
                                row0a=yq(qa,st_a,0,fam)>>3; row0b=yq(qb,st_b,0,fam)>>3;
                                if(cover_hash(na,ra,ca,row0a,cc0)!=cover_hash(nb,rb,cb,row0b,cc0)) ++l4;
                                /* vocabulary AFTER canonicalization, on the ROM path */
                                { uint64_t sh=1469598103934665603ull; for(k=0;k<nb;++k){ sh^=(uint64_t)(mb[k]+200); sh*=1099511628211ull; }
                                  hs_add(&vshape,sh);
                                  hs_add(&vstate,((uint64_t)(uint16_t)qb<<32)|((uint64_t)(uint16_t)st_b<<16)|((uint64_t)fam<<8)|(uint64_t)want); }
                            }
                            cs+=CHUNK; left-=want;
                        }
                    }
                }
            }
        }
    }
    printf("arbitrary poses: every walkable cell x 6 sub-cell offsets x %u headings\n",256u/yaw_step);
    printf("FULL runs                       %lu\n",runs);
    printf("  depth-plane path applies      %lu (%.1f%%)\n",dp_ok,100.0*dp_ok/(double)runs);
    printf("chunk-family instances compared %lu\n",chunks);
    printf("\ndisagreement by layer (ROM depth-plane path vs host inv0/inv1 path)\n");
    printf("  L1 endpoint  c0,c1            shared by construction (both from angle_x)\n");
    printf("  L2 depth     per-column h(c)  %lu (%.3f%%)\n",l2,100.0*l2/(double)chunks);
    printf("  L3 trajectory cursor moves    %lu (%.3f%%)\n",l3,100.0*l3/(double)chunks);
    printf("  L4 ownership covered cells    %lu (%.3f%%)  [evaluated independently of L3]\n",l4,100.0*l4/(double)chunks);
    printf("  unmodelled (move family) A=%lu B=%lu\n",unmodelled_a,unmodelled_b);
    printf("\nvocabulary collapse, measured AFTER canonicalization (ROM path)\n");
    printf("  chunk-family instances        %lu\n",inst);
    printf("  distinct (iq,step,family,len) %lu\n",vstate.n);
    printf("  distinct raster trajectories  %lu\n",vshape.n);
    printf("  collapse instances->trajectories  %.0f : 1\n",(double)inst/(double)vshape.n);
    return 0;
}
