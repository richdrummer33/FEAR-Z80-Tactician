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

static int g_bad_jump,g_bad_want,g_bad_col;
static int g_ctx_px,g_ctx_py,g_ctx_yaw;
#define WMAX 4000
static FILE *g_wf=NULL; static int g_wn=0; static FILE *g_growth=NULL;
#define JMIN (-40)
#define JMAX 8
static unsigned long jhist[JMAX-JMIN+1];
static unsigned long jhist_fam[JMAX-JMIN+1][3];
static unsigned long jwant[JMAX-JMIN+1][8];

/* canonical raster shape; returns cell count, fills moves[] and rows[]/cols[] */
/* The depth term is a uint8: h = (((iq + c*step + 32)>>6) & 0xFF) >> 1. Evaluating
 * a column where the pre-mask value leaves [0,255] wraps it, which shows up as a
 * spurious ~127px (15 row) leap. That is a domain violation, not geometry, so it
 * is reported rather than modelled. Returns -2 for it, -1 for a genuine
 * out-of-family jump. */
static int depth_in_domain(int16_t iq,int16_t step,int c){
    int raw=(iq+c*step+32)>>6; return raw>=0&&raw<=255;
}
static int shape_of(int16_t iq,int16_t step,int fam,int want,int *moves,int *rows,int *cols,int is_last)
{
    int span_lo[CHUNK+2],span_hi[CHUNK+2],c,n=0,row,col=0;
    /* The exit move needs one column past the chunk. The final chunk of a run has
     * no successor, so that column lies outside the run and the depth model has no
     * business being evaluated there: such a chunk terminates instead. */
    int ncols = is_last ? want+1 : want+2;
    for(c=0;c<ncols;++c) if(!depth_in_domain(iq,step,c)) return -2;
    for(c=0;c<(is_last?want:want+1);++c){
        int a=yq(iq,step,c,fam),b=yq(iq,step,c+1,fam);
        int lo=a<=b?a:b, hi=a<=b?b:a;
        span_lo[c]=lo>>3; span_hi[c]=hi>>3;
    }
    row=span_lo[0];
    for(c=0;c<want;++c){
        int r0=span_lo[c],r1=span_hi[c],k,jump;
        for(k=0;k<r1-r0;++k){ rows[n]=row; cols[n]=col; moves[n]=39; ++n; ++row; }
        if(is_last&&c==want-1){ rows[n]=row; cols[n]=col; moves[n]=0; ++n; break; }  /* terminator */
        jump=span_lo[c+1]-r1;
        if((jump<-4&&jump!=-14&&jump!=-15)||jump>0){ g_bad_jump=jump; g_bad_want=want; g_bad_col=c; return -1; }
        rows[n]=row; cols[n]=col; moves[n]= jump==0?1:(jump==-1?-39:(jump==-2?-79:(jump==-3?-119:(jump==-4?-159:(jump==-14?-559:-599)))));
        if((jump==-14||jump==-15)&&g_wf&&g_wn<WMAX){
            fprintf(g_wf,"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                    jump,(int)iq,(int)step,fam,want,c,
                    yq(iq,step,c,fam),yq(iq,step,c+1,fam),yq(iq,step,c+2,fam),
                    r1,span_lo[c+1],g_ctx_px,g_ctx_py,g_ctx_yaw);
            ++g_wn; }
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

/* Unique trajectories retained so equivalence and byte cost can be measured. */
#define TMAX 20000
#define TLEN 96
static signed short tmoves[TMAX][TLEN]; static int tlen[TMAX]; static int tfam[TMAX]; static int ntraj=0;
static unsigned long tcount[TMAX];
#define TIBITS 18
#define TISIZE (1u<<TIBITS)
static int tidx[TISIZE]; static uint64_t tkey[TISIZE]; static int tidx_init=0;
static void traj_keep(const int *mv,int n,int fam,uint64_t h){
    unsigned i; int j;
    if(n>TLEN) return;
    if(!tidx_init){ for(i=0;i<TISIZE;++i) tidx[i]=-1; tidx_init=1; }
    i=(unsigned)((h^(h>>29))&(TISIZE-1u));
    for(;;){ if(tidx[i]<0) break;
             if(tkey[i]==h){ int k=tidx[i]; tfam[k]|=(fam==0?1:2); ++tcount[k]; return; }
             i=(i+1u)&(TISIZE-1u); }
    if(ntraj<TMAX){ for(j=0;j<n;++j) tmoves[ntraj][j]=(signed short)mv[j];
                    tlen[ntraj]=n; tfam[ntraj]=(fam==0?1:2); tcount[ntraj]=1;
                    tidx[i]=ntraj; tkey[i]=h; ++ntraj; }
}
#define MOVE_N 8
static const int k_moves[MOVE_N]={39,1,-39,-79,-119,-159,0,-599};  /* 0 = run terminator */
static int move_slot(int m){ int i; for(i=0;i<MOVE_N;++i) if(k_moves[i]==m) return i; return -1; }
static unsigned long mv_occ[MOVE_N]; static unsigned long mv_traj[MOVE_N];

int main(int argc,char**argv)
{
    unsigned yaw_step = argc>1 ? (unsigned)strtoul(argv[1],0,0) : 1u;
    /* Exhaustive mode: sweep the FULL 64x64 local translational space of every
     * Nth walkable cell, rather than a handful of sub-cell offsets. Prints the
     * running trajectory total after each cell, which is the growth curve: if
     * the vocabulary is finite it must flatten. */
    unsigned exh = argc>2 ? (unsigned)strtoul(argv[2],0,0) : 0u;
    unsigned cellno=0;
    TSPState s; unsigned gx,gy,yaw,oi,i,c;
    static const int8_t off[][2]={{0,0},{7,3},{3,7},{11,5},{19,23},{41,37}};
    unsigned long runs=0,dp_ok=0,chunks=0;
    unsigned long l1=0,l2=0,l3=0,l4=0,unmodelled_a=0,unmodelled_b=0,domain_a=0,domain_b=0;
    unsigned long inst=0;
    vstate=hs_new(); vshape=hs_new();
    {   const char *od=argc>3?argv[3]:NULL; char pth[512];
        if(od){ snprintf(pth,sizeof pth,"%s/weird_moves.csv",od); g_wf=fopen(pth,"w");
                if(g_wf) fprintf(g_wf,"jump,iq,step,fam,want,col,y_c,y_c1,y_c2,row_hi,next_lo,px,py,yaw\n");
                snprintf(pth,sizeof pth,"%s/growth.csv",od); g_growth=fopen(pth,"w");
                if(g_growth) fprintf(g_growth,"cell,instances,trajectories,states\n"); } }
    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t px0=(int16_t)(gx*CELL_Q4+32),py0=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(px0,py0)) continue;
        if(exh){ ++cellno; if((cellno-1u)%exh) continue; }
        for(oi=0;oi<(exh?4096u:6u);++oi){
            int16_t px,py;
            if(exh){ px=(int16_t)(gx*CELL_Q4+(int)(oi&63u)); py=(int16_t)(gy*CELL_Q4+(int)(oi>>6)); }
            else   { px=(int16_t)(px0+off[oi][0]); py=(int16_t)(py0+off[oi][1]); }
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
                            g_ctx_px=px; g_ctx_py=py; g_ctx_yaw=(int)yaw;
                            na=shape_of(qa,st_a,fam,want,ma,ra,ca,left-want==0);
                            nb=shape_of(qb,st_b,fam,want,mb,rb,cb,left-want==0);
                            if(na==-2) ++domain_a; else if(na<0) ++unmodelled_a;
                            if(nb==-2) ++domain_b;
                            else if(nb<0){
                                ++unmodelled_b;
                                if(g_bad_jump>=JMIN&&g_bad_jump<=JMAX){
                                    ++jhist[g_bad_jump-JMIN];
                                    ++jhist_fam[g_bad_jump-JMIN][fam];
                                    if(g_bad_want<8) ++jwant[g_bad_jump-JMIN][g_bad_want];
                                }
                            }
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
                                  hs_add(&vshape,sh); traj_keep(mb,nb,fam,sh);
                                  for(k=0;k<nb;++k){int sl=move_slot(mb[k]); if(sl>=0) ++mv_occ[sl];}
                                  hs_add(&vstate,((uint64_t)(uint16_t)qb<<32)|((uint64_t)(uint16_t)st_b<<16)|((uint64_t)fam<<8)|(uint64_t)want); }
                            }
                            cs+=CHUNK; left-=want;
                        }
                    }
                }
            }
        }
        if(exh && ((cellno-1u)%exh)==0u){
            printf("  growth: after cell %-5u  trajectories=%-6lu states=%-9lu instances=%lu\n",
                   cellno,vshape.n,vstate.n,inst);
            if(g_growth){ fprintf(g_growth,"%u,%lu,%lu,%lu\n",cellno,inst,vshape.n,vstate.n); fflush(g_growth); } }
    }
    if(exh) printf("\nEXHAUSTIVE local sweep: full 64x64 translations of every %uth walkable cell\n",exh);
    else    printf("arbitrary poses: every walkable cell x 6 sub-cell offsets x %u headings\n",256u/yaw_step);
    printf("FULL runs                       %lu\n",runs);
    printf("  depth-plane path applies      %lu (%.1f%%)\n",dp_ok,100.0*dp_ok/(double)runs);
    printf("chunk-family instances compared %lu\n",chunks);
    printf("\ndisagreement by layer (ROM depth-plane path vs host inv0/inv1 path)\n");
    printf("  L1 endpoint  c0,c1            shared by construction (both from angle_x)\n");
    printf("  L2 depth     per-column h(c)  %lu (%.3f%%)\n",l2,100.0*l2/(double)chunks);
    printf("  L3 trajectory cursor moves    %lu (%.3f%%)\n",l3,100.0*l3/(double)chunks);
    printf("  L4 ownership covered cells    %lu (%.3f%%)  [evaluated independently of L3]\n",l4,100.0*l4/(double)chunks);
    printf("  out-of-family jump       A=%lu B=%lu\n",unmodelled_a,unmodelled_b);
    printf("  depth term outside uint8 A=%lu (%.3f%%) B=%lu (%.3f%%)  [domain violation, not geometry]\n",
           domain_a,100.0*domain_a/(double)chunks,domain_b,100.0*domain_b/(double)chunks);
    printf("\ninexpressible cursor moves on the ROM path, by required row jump\n");
    printf("  (the compiled-body family is jump 0..-4; anything else cannot be a body)\n");
    {
        unsigned long tot=0,cum=0; int j,order[JMAX-JMIN+1],m,n2=0,k2;
        for(j=JMIN;j<=JMAX;++j) if(jhist[j-JMIN]) { tot+=jhist[j-JMIN]; order[n2++]=j; }
        for(m=0;m<n2;++m) for(k2=m+1;k2<n2;++k2)
            if(jhist[order[k2]-JMIN]>jhist[order[m]-JMIN]){int s2=order[m];order[m]=order[k2];order[k2]=s2;}
        printf("  %-8s %-12s %-9s %-22s %s\n","jump","count","share","family (top/bot)","cumulative if added");
        for(m=0;m<n2;++m){
            j=order[m]; cum+=jhist[j-JMIN];
            printf("  %-8d %-12lu %-8.3f%% top=%-7lu bot=%-7lu  %.4f%% of all chunks remain unexpressible\n",
                   j,jhist[j-JMIN],100.0*jhist[j-JMIN]/(double)tot,
                   jhist_fam[j-JMIN][0],jhist_fam[j-JMIN][2],
                   100.0*(unmodelled_b-cum)/(double)chunks);
        }
        printf("  total inexpressible %lu of %lu chunks (%.3f%%)\n",tot,chunks,100.0*tot/(double)chunks);
    }
    printf("\nvocabulary collapse, measured AFTER canonicalization (ROM path)\n");
    printf("  chunk-family instances        %lu\n",inst);
    printf("  distinct (iq,step,family,len) %lu\n",vstate.n);
    printf("  distinct raster trajectories  %lu\n",vshape.n);
    printf("  collapse instances->trajectories  %.0f : 1\n",(double)inst/(double)vshape.n);
    {
        int i,j; long total_moves=0,maxlen=0; int only_top=0,only_bot=0,shared=0;
        HSet rev=hs_new();
        for(i=0;i<ntraj;++i){
            total_moves+=tlen[i]; if(tlen[i]>maxlen) maxlen=tlen[i];
            if(tfam[i]==1) ++only_top; else if(tfam[i]==2) ++only_bot; else ++shared;
        }
        for(i=0;i<ntraj;++i){ uint64_t h=1469598103934665603ull;
            for(j=tlen[i]-1;j>=0;--j){ h^=(uint64_t)(tmoves[i][j]+700); h*=1099511628211ull; }
            hs_add(&rev,h); }
        printf("\ndeduplicated generic trajectory programs\n");
        printf("  unique trajectories retained  %d\n",ntraj);
        printf("  top family only               %d\n",only_top);
        printf("  bottom family only            %d\n",only_bot);
        printf("  BOTH families                 %d   (already shared, no mirroring needed)\n",shared);
        printf("  total moves across all        %ld\n",total_moves);
        printf("  mean / max length             %.1f / %ld moves\n",(double)total_moves/(double)ntraj,maxlen);
        printf("  bytes at 1 byte per move      %ld\n",total_moves);
        printf("  bytes at 3 bits per move      %ld   (7 moves fit in 3 bits)\n",(total_moves*3+7)/8);
        printf("  distinct under reversal       %lu of %d\n",rev.n,ntraj);
        if(argc>3){
            char pth[512]; FILE *f; int m;
            for(i=0;i<ntraj;++i){ int sl; for(j=0;j<tlen[i];++j){ sl=move_slot(tmoves[i][j]);
                    if(sl>=0&&(j==0||1)) { } } }
            for(i=0;i<ntraj;++i){ int seen[MOVE_N]; for(m=0;m<MOVE_N;++m) seen[m]=0;
                for(j=0;j<tlen[i];++j){ int sl=move_slot(tmoves[i][j]); if(sl>=0) seen[sl]=1; }
                for(m=0;m<MOVE_N;++m) if(seen[m]) ++mv_traj[m]; }
            snprintf(pth,sizeof pth,"%s/trajectories.csv",argv[3]); f=fopen(pth,"w");
            if(f){ fprintf(f,"id,count,length,fams\n");
                   for(i=0;i<ntraj;++i) fprintf(f,"%d,%lu,%d,%d\n",i,tcount[i],tlen[i],tfam[i]);
                   fclose(f); }
            snprintf(pth,sizeof pth,"%s/moves.csv",argv[3]); f=fopen(pth,"w");
            if(f){ fprintf(f,"move,rowjump,occurrences,trajectories_containing\n");
                   for(m=0;m<MOVE_N;++m){ int rj = k_moves[m]==39?99:(k_moves[m]==1?0:-(( -k_moves[m]+1)/40));
                       fprintf(f,"%d,%d,%lu,%lu\n",k_moves[m],rj,mv_occ[m],mv_traj[m]); }
                   fclose(f); }
            if(g_wf) fclose(g_wf);
            if(g_growth) fclose(g_growth);
        }
    }
    return 0;
}
