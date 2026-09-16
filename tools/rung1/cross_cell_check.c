/* Rung 2: does the pose-independent representation survive a coarse-cell crossing?
 *
 * Rung 1 established that arbitrary pose -> ROM projection -> iq/step -> closed
 * form raster state works, and that screen_depth_plane is the faithful
 * derivation. All of that was measured WITHIN a coarse cell. The projection's
 * front half is a per-coarse-cell baked field: each 64x64 Q4 cell carries its
 * own quadtree of planar corner-bearing fits. Crossing a cell boundary swaps
 * the whole record set. If the two sides disagree, every crossing manufactures
 * a raster event that the geometry never produced -- fatal for a temporal or
 * certificate renderer, whatever the static accuracy.
 *
 * The question is NOT "does anything change when the player moves" -- of course
 * it does. It is whether a boundary step changes MORE than an ordinary step in
 * open space does. So every step is classified by what it crosses and the same
 * measurements are reported per class:
 *
 *   interior      no corner changes leaf, same coarse cell
 *   leaf          at least one corner changes quadtree leaf, same coarse cell
 *   cell          the coarse cell itself changes: a different baked record set
 *
 * and at four layers, each against the exact bearing_q12 path evaluated at the
 * same two poses, so that real motion is subtracted out:
 *
 *   L0 corner bearing   spurious part of the baked change, in Q12 units
 *   L1 screen endpoint  phantom/missed column transitions
 *   L3 trajectory       phantom/missed canonical raster-state transitions
 *   L4 ownership        phantom/missed changes to the covered cell set
 *
 * A phantom transition is the one that matters: the baked path moves to a new
 * raster state where the exact path stays put.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tilesector_polar_renderer.c"
#include "local_projection_host.h"
#include "dp_tables.h"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define CHUNK 6
#define NT_ROWS 18
#define MAXRUN 24

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
static int in_domain(int16_t iq,int16_t step,int c){ int raw=(iq+c*step+32)>>6; return raw>=0&&raw<=255; }
static int hq(int16_t iq,int16_t step,int c){ return (int)((((iq+c*step+32)>>6)&0xFF)>>1); }
static int yq(int16_t iq,int16_t step,int c,int fam){ int h=hq(iq,step,c); return fam==0?71-h:72+h; }

/* one run's canonical raster state: the move stream of every chunk, and the
 * cells it covers. Identical in construction to the Rung 1 harness. */
/* 18 rows x 20 columns of name table, as an explicit bitmap rather than a hash,
 * so the SIZE of a disagreement can be measured and not just its incidence.
 * Incidence alone was what made the depth-layer numbers misleading before. */
#define COVW 6
typedef struct {
    uint8_t sid,c0,c1,ok;
    uint8_t x0,x1;      /* screen pixels, finer than the tile column */
    uint64_t traj;      /* hash of the move stream over both families */
    uint64_t cover[COVW];
} RunState;
static int cov_popdiff(const uint64_t *a,const uint64_t *b)
{
    int i,n=0; for(i=0;i<COVW;++i){ uint64_t x=a[i]^b[i];
        while(x){ x&=x-1; ++n; } } return n;
}
static int cov_eq(const uint64_t *a,const uint64_t *b)
{ int i; for(i=0;i<COVW;++i) if(a[i]!=b[i]) return 0; return 1; }
static void cov_set(uint64_t *c,int row,int col)
{ int b; if(row<0||row>=NT_ROWS||col<0||col>=20) return; b=row*20+col; c[b>>6]|=1ull<<(b&63); }
typedef struct { int n; RunState r[MAXRUN]; } Frame;

static void chunk_state(int16_t iq,int16_t step,int fam,int want,int is_last,
                        int c0abs,uint64_t *traj,uint64_t *cover,int *ok)
{
    int lo[CHUNK+2],hi[CHUNK+2],c,row,col=0,ncols=is_last?want+1:want+2;
    for(c=0;c<ncols;++c) if(!in_domain(iq,step,c)){ *ok=0; return; }
    for(c=0;c<(is_last?want:want+1);++c){
        int a=yq(iq,step,c,fam),b=yq(iq,step,c+1,fam);
        lo[c]=(a<=b?a:b)>>3; hi[c]=(a<=b?b:a)>>3;
    }
    row=lo[0];
    for(c=0;c<want;++c){
        int k,jump;
        for(k=0;k<hi[c]-lo[c];++k){
            *traj^=39u; *traj*=1099511628211ull;
            cov_set(cover,row,c0abs+col);
            ++row;
        }
        cov_set(cover,row,c0abs+col);
        if(is_last&&c==want-1){ *traj^=200u; *traj*=1099511628211ull; break; }
        jump=lo[c+1]-hi[c];
        if(jump>0||jump<-4){ *ok=0; return; }
        *traj^=(uint64_t)(100+jump); *traj*=1099511628211ull;
        ++col; row+=jump;
    }
}

/* Evaluate one pose under one of three corner-bearing sources.
 *
 *   PATH_BAKE   the ROM's baked local-projection field
 *   PATH_INT    bearing_q12, the shipped integer atan2
 *   PATH_TRUE   continuous geometry, quantised to Q12 only at the last step
 *
 * The first version of this harness compared BAKE against INT and called INT
 * "exact". It is not: ratio_q8_exact(n,n) returns 0, so bearing_q12 reports an
 * axis direction for near-diagonal corners, and every one of those would have
 * been charged to the bake as a phantom event. Both integer paths are now
 * scored against continuous truth, the same way the depth derivations were. */
#define PATH_BAKE 0
#define PATH_INT  1
#define PATH_TRUE 2
static uint16_t true_bearing_q12(unsigned v,int16_t px,int16_t py)
{
    double dx=(double)(((int16_t)k_tspf_vx[v]<<4)-px);
    double dy=(double)(((int16_t)k_tspf_vy[v]<<4)-py);
    double a;
    if(dx==0.0&&dy==0.0) return 0u;
    a=atan2(dy,dx)*4096.0/(2.0*M_PI);
    while(a<0.0) a+=4096.0;
    return (uint16_t)(((long)floor(a+0.5))&4095L);
}
static void sample(int16_t px,int16_t py,unsigned yaw,unsigned gx,unsigned gy,
                   int path,Frame *f)
{
    TSPState s; uint8_t ks[64],nk=0,count=0,j;
    uint8_t recipe,base_id,cond_count,lx,ly; uint16_t gi,offs; unsigned i,c;
    const uint8_t *p,*b;
    memset(f,0,sizeof *f);
    memset(&s,0,sizeof s); s.x_q4=px; s.y_q4=py; s.yaw=(uint8_t)yaw;
    gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);
    recipe=k_tspf_recipe_grid[gi]; if(recipe==0xffu) return;
    lx=(uint8_t)((uint16_t)px&63u); ly=(uint8_t)((uint16_t)py&63u);
    offs=k_tspf_recipe_off[recipe]; p=&k_tspf_recipe_stream[offs];
    base_id=*p++; cond_count=*p++;
    b=&k_tspf_base_stream[k_tspf_base_off[base_id]]; i=*b++;
    for(;i;--i) ks[nk++]=*b++;
    for(i=0;i<cond_count;++i){ uint8_t key=*p++,sel=*p++; if(selector_pass(sel,lx,ly)) ks[nk++]=key; }
    tsp_polar_renderer_reset(); g_corner_bearing_valid=0u;
    if(path==PATH_BAKE){
        unsigned v;
        proj_host_load(gx,gy);
        for(v=0;v<14u;++v){
            uint16_t baked;
            if(proj_host_bearing(v,px,py,&baked)){
                g_corner_bearing_q12[v]=baked;
                g_corner_bearing_valid|=k_corner_mask[v];
            }
            /* absent or fallback corners keep bearing_q12, exactly as the ROM's
             * projection_eval_fallback does */
        }
    } else if(path==PATH_TRUE){
        unsigned v;
        for(v=0;v<14u;++v){
            g_corner_bearing_q12[v]=true_bearing_q12(v,px,py);
            g_corner_bearing_valid|=k_corner_mask[v];
        }
    }
    for(j=0;j<nk;++j){ if(count>=TSPF_MAX_ACTIVE) break;
        if(!project_key(ks[j],&s,&g_runs[count])) continue;
        insert_run(count,&count); }
    for(c=0;c<count&&f->n<MAXRUN;++c){
        PolarRun *r=&g_runs[c];
        uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),n,invd;
        int16_t iq,step; int fam,left,cs; RunState *o;
        if(cc0>=TSP_COLS) cc0=TSP_COLS-1;
        if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
        if(cc1<cc0) continue;
        if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
        n=(uint8_t)(cc1-cc0+1u);
        invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],&s));
        o=&f->r[f->n++];
        o->sid=r->sid; o->c0=cc0; o->c1=cc1; o->ok=1;
        o->x0=r->x0; o->x1=r->x1;
        o->traj=1469598103934665603ull; memset(o->cover,0,sizeof o->cover);
        if(!dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&iq,&step)){ o->ok=0; continue; }
        for(fam=0;fam<=2;fam+=2){
            int okf=1; left=n; cs=0;
            while(left>0&&okf){
                int want=left>CHUNK?CHUNK:left;
                int16_t q=(int16_t)(iq+(int16_t)(cs*step));
                chunk_state(q,step,fam,want,left-want==0,cc0+(cs),&o->traj,o->cover,&okf);
                cs+=CHUNK; left-=want;
            }
            if(!okf) o->ok=0;
        }
    }
}

/* which corners changed quadtree leaf between two positions in the same cell */
static int leaf_changed(unsigned gx,unsigned gy,int16_t ax,int16_t ay,int16_t bx,int16_t by)
{
    unsigned v; int changed=0;
    proj_host_load(gx,gy);
    for(v=0;v<14u;++v){
        unsigned sh,n;
        if(g_pj_depth[v]==0xffu) continue;
        sh=6u-g_pj_depth[v]; n=1u<<g_pj_depth[v];
        if( ((((unsigned)ay&63u)>>sh)*n + (((unsigned)ax&63u)>>sh)) !=
            ((((unsigned)by&63u)>>sh)*n + (((unsigned)bx&63u)>>sh)) ) changed=1;
    }
    return changed;
}

enum { CLS_INTERIOR=0, CLS_LEAF=1, CLS_CELL=2, CLS_N=3 };
static const char *CLSN[CLS_N]={"interior","leaf boundary","CELL BOUNDARY"};
#define NPATH 2
static const char *PATHN[NPATH]={"baked field","bearing_q12"};
static unsigned long steps[CLS_N],cmp_runs[NPATH][CLS_N];
static unsigned long ph_end[NPATH][CLS_N],ms_end[NPATH][CLS_N];
static unsigned long ph_traj[NPATH][CLS_N],ms_traj[NPATH][CLS_N];
static unsigned long ph_cov[NPATH][CLS_N],ms_cov[NPATH][CLS_N];
static unsigned long st_end[NPATH][CLS_N],st_traj[NPATH][CLS_N],st_cov[NPATH][CLS_N];
/* How BIG is a manufactured transition, not just how often. A 2% incidence of
 * one-pixel-early threshold crossings is a different result from a 2% incidence
 * of five-pixel jumps, and incidence alone cannot tell them apart. */
#define MAGB 33
static unsigned long mag_px[NPATH][CLS_N][MAGB];    /* |dx0|+|dx1| on the phantom step */
static unsigned long mag_cells[NPATH][CLS_N][MAGB]; /* cells added+removed by it */
static void mag_add(unsigned long *h,int v){ if(v<0)v=0; if(v>=MAGB)v=MAGB-1; ++h[v]; }
static double mag_pct(const unsigned long *h,double q)
{
    unsigned long tot=0,acc=0; int i;
    for(i=0;i<MAGB;++i) tot+=h[i];
    if(!tot) return 0.0;
    for(i=0;i<MAGB;++i){ acc+=h[i]; if((double)acc>=q*(double)tot) return (double)i; }
    return MAGB-1;
}
static unsigned long mag_atmost(const unsigned long *h,int k)
{ unsigned long a=0; int i; for(i=0;i<=k&&i<MAGB;++i) a+=h[i]; return a; }
static unsigned long mag_total(const unsigned long *h)
{ unsigned long a=0; int i; for(i=0;i<MAGB;++i) a+=h[i]; return a; }

static const RunState *find(const Frame *f,uint8_t sid)
{ int i; for(i=0;i<f->n;++i) if(f->r[i].sid==sid) return &f->r[i]; return 0; }

static void compare(int pi,int cls,const Frame *ba,const Frame *bb,
                    const Frame *ea,const Frame *eb)
{
    int i;
    for(i=0;i<ea->n;++i){
        const RunState *e0=&ea->r[i],*e1=find(eb,e0->sid);
        const RunState *b0=find(ba,e0->sid),*b1=find(bb,e0->sid);
        int ec,bc;
        if(!e1||!b0||!b1) continue;
        if(!e0->ok||!e1->ok||!b0->ok||!b1->ok) continue;
        ++cmp_runs[pi][cls];
        if(b0->c0!=e0->c0||b0->c1!=e0->c1) ++st_end[pi][cls];
        if(b0->traj!=e0->traj) ++st_traj[pi][cls];
        if(!cov_eq(b0->cover,e0->cover)) ++st_cov[pi][cls];
        ec=(e0->c0!=e1->c0||e0->c1!=e1->c1); bc=(b0->c0!=b1->c0||b0->c1!=b1->c1);
        if(bc&&!ec){ ++ph_end[pi][cls];
            mag_add(mag_px[pi][cls],abs((int)b1->x0-(int)b0->x0)+abs((int)b1->x1-(int)b0->x1)); }
        else if(ec&&!bc) ++ms_end[pi][cls];
        ec=(e0->traj!=e1->traj); bc=(b0->traj!=b1->traj);
        if(bc&&!ec) ++ph_traj[pi][cls]; else if(ec&&!bc) ++ms_traj[pi][cls];
        ec=!cov_eq(e0->cover,e1->cover); bc=!cov_eq(b0->cover,b1->cover);
        if(bc&&!ec){ ++ph_cov[pi][cls];
            mag_add(mag_cells[pi][cls],cov_popdiff(b0->cover,b1->cover)); }
        else if(ec&&!bc) ++ms_cov[pi][cls];
    }
}

/* ---- arm 0: the transcription must reproduce the bake's certified accuracy - */
/* The bake was fitted against float atan2, not against bearing_q12, so that is
 * what it must be scored on. Scoring it against bearing_q12 instead produced a
 * worst error of 515 Q12 units and looked like a broken transcription; it was
 * not. Those cases were the ratio_q8_exact(n,n) == 0 wrap, where bearing_q12
 * reported an axis direction 45 degrees away and the bake was right. That wrap
 * is now fixed, and this arm is kept as the place the difference would reappear.
 * Both comparisons are reported. */
static double wrap12d(double a){ while(a>2048.0)a-=4096.0; while(a<-2048.0)a+=4096.0; return a; }
static int arm_transcription(void)
{
    unsigned gx,gy,v,lx,ly; unsigned long n=0,fb=0,cells=0,diag=0,diagbad=0;
    double worst_true=0.0; int worst_q12=0, worst_q12_nodiag=0;
    printf("0  host transcription of the baked field\n");
    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        if(!proj_host_load(gx,gy)) continue;
        ++cells;
        for(v=0;v<14u;++v){
            if(g_pj_depth[v]==0xffu){ if(g_pj_fallback_mask&k_corner_mask[v]) ++fb; continue; }
            for(ly=0;ly<64u;ly+=3u) for(lx=0;lx<64u;lx+=3u){
                int16_t px=(int16_t)(gx*CELL_Q4+(int)lx),py=(int16_t)(gy*CELL_Q4+(int)ly);
                uint16_t baked,exact; int d,dx,dy,ax,ay; double truth,e;
                if(!proj_host_bearing(v,px,py,&baked)) continue;
                dx=(int)(((int16_t)k_tspf_vx[v]<<4)-px);
                dy=(int)(((int16_t)k_tspf_vy[v]<<4)-py);
                if(!dx&&!dy) continue;
                ax=dx<0?-dx:dx; ay=dy<0?-dy:dy;
                truth=atan2((double)dy,(double)dx)*4096.0/(2.0*M_PI);
                e=fabs(wrap12d((double)baked-truth));
                if(e>worst_true) worst_true=e;
                exact=bearing_q12((int16_t)dx,(int16_t)dy);
                d=(int)((baked-exact)&4095u); if(d>2048) d-=4096; if(d<0) d=-d;
                if(d>worst_q12) worst_q12=d;
                if(ax==ay){ ++diag; if(d>8) ++diagbad; }
                else if(d>worst_q12_nodiag) worst_q12_nodiag=d;
                ++n;
            }
        }
    }
    printf("   %lu baked cells, %lu fallback corners, %lu samples\n",cells,fb,n);
    printf("   vs float atan2 (what the bake was fitted to)   worst %.2f Q12 (%.3f px)   %s\n",
           worst_true,worst_true*160.0/1024.0,worst_true<=8.0?"ok":"SUSPECT");
    printf("   vs bearing_q12, excluding exact diagonals       worst %d Q12 (%.3f px)\n",
           worst_q12_nodiag,(double)worst_q12_nodiag*160.0/1024.0);
    printf("   vs bearing_q12, all samples                     worst %d Q12 (%.3f px)\n",
           worst_q12,(double)worst_q12*160.0/1024.0);
    printf("   %lu samples whose scaled operands are equal, %lu off by more than 8 Q12.\n",diag,diagbad);
    printf("   These used to be the ratio_q8_exact(n,n) == 0 wrap, where bearing_q12\n");
    printf("   reported an axis direction 45 degrees away and the bake was the correct\n");
    printf("   one. That is now fixed by saturating the ratio, and the residue here is\n");
    printf("   ordinary quantisation.\n");
    if(worst_true>8.0){
        printf("   the bake was emitted at a threshold of 4 Q12 units; a worst error far\n");
        printf("   above that against its own fitting target means this transcription is\n");
        printf("   wrong, not the bake.\n");
        return 1;
    }
    return 0;
}

int main(int argc,char**argv)
{
    unsigned yaw_step=argc>1?(unsigned)strtoul(argv[1],0,0):8u;
    unsigned gx,gy,yaw,t,k;
    /* step offsets chosen to hit each class: 63->64 leaves the coarse cell,
     * 31/15/7 are quadtree leaf boundaries at depth 1/2/3, the rest are open. */
    static const int OFFS[]={63,31,15,7,5,21,43,58};
    static const int NOFF=(int)(sizeof(OFFS)/sizeof(OFFS[0]));
    unsigned long loads=0,load_bytes=0;
    Frame ba,bb,ea,eb;

    if(arm_transcription()) return 1;

    printf("\n1  transition behaviour of the baked field, by what the step crosses\n");
    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t c0x=(int16_t)(gx*CELL_Q4+32),c0y=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(c0x,c0y)) continue;
        if(!proj_host_load(gx,gy)) continue;
        ++loads; load_bytes+=g_pj_cell_bytes;
        for(t=0;t<(unsigned)NOFF;++t) for(k=0;k<2u;++k){
            /* k=0 steps in x, k=1 in y */
            int o=OFFS[t];
            int16_t ax=(int16_t)(gx*CELL_Q4+(k?32:o)), ay=(int16_t)(gy*CELL_Q4+(k?o:32));
            int16_t bx=(int16_t)(ax+(k?0:1)),          by=(int16_t)(ay+(k?1:0));
            unsigned ngx=(unsigned)(bx/CELL_Q4),ngy=(unsigned)(by/CELL_Q4);
            int cls;
            if(!tsp_is_walkable_q4(ax,ay)||!tsp_is_walkable_q4(bx,by)) continue;
            if(ngx!=gx||ngy!=gy){ cls=CLS_CELL; if(!proj_host_load(ngx,ngy)) continue; }
            else cls=leaf_changed(gx,gy,ax,ay,bx,by)?CLS_LEAF:CLS_INTERIOR;
            for(yaw=0;yaw<256u;yaw+=yaw_step){
                Frame ia,ib;
                ++steps[cls];
                sample(ax,ay,yaw,gx,gy,PATH_TRUE,&ea);
                sample(bx,by,yaw,ngx,ngy,PATH_TRUE,&eb);
                sample(ax,ay,yaw,gx,gy,PATH_BAKE,&ba);
                sample(bx,by,yaw,ngx,ngy,PATH_BAKE,&bb);
                compare(PATH_BAKE,cls,&ba,&bb,&ea,&eb);
                sample(ax,ay,yaw,gx,gy,PATH_INT,&ia);
                sample(bx,by,yaw,ngx,ngy,PATH_INT,&ib);
                compare(PATH_INT,cls,&ia,&ib,&ea,&eb);
            }
        }
    }

    printf("   Both integer paths are scored against a reference path: the identical\n");
    printf("   raster pipeline driven by continuous-truth bearings, quantised to Q12\n");
    printf("   only at the bearing step. That isolates the corner-bearing source,\n");
    printf("   which is what a coarse-cell crossing actually changes.\n\n");
    printf("   %-14s %10s %12s\n","step class","steps","run pairs");
    for(t=0;t<CLS_N;++t)
        printf("   %-14s %10lu %12lu\n",CLSN[t],steps[t],cmp_runs[0][t]);
    {
        int pi;
        printf("\n   APPROXIMATION-INDUCED transitions: the integer path changes quantised\n");
        printf("   raster state on this step although the reference path -- the SAME raster\n");
        printf("   pipeline driven by continuous-truth bearings -- did not cross the\n");
        printf("   corresponding raster boundary. The projected geometry of course moves on\n");
        printf("   every step; what is counted is a state change the reference never called for.\n");
        for(pi=0;pi<NPATH;++pi){
            printf("    %s\n",PATHN[pi]);
            printf("     %-14s %14s %14s %14s\n","step class","L1 endpoint","L3 trajectory","L4 ownership");
            for(t=0;t<CLS_N;++t){
                double d=(double)(cmp_runs[pi][t]?cmp_runs[pi][t]:1);
                printf("     %-14s %8lu %5.3f%% %8lu %5.3f%% %8lu %5.3f%%\n",CLSN[t],
                       ph_end[pi][t],100.0*ph_end[pi][t]/d,ph_traj[pi][t],100.0*ph_traj[pi][t]/d,
                       ph_cov[pi][t],100.0*ph_cov[pi][t]/d);
            }
        }
        printf("\n   MISSED transitions: the reference path crosses a raster boundary\n");
        printf("   and the integer path does not\n");
        for(pi=0;pi<NPATH;++pi){
            printf("    %s\n",PATHN[pi]);
            for(t=0;t<CLS_N;++t){
                double d=(double)(cmp_runs[pi][t]?cmp_runs[pi][t]:1);
                printf("     %-14s %8lu %5.3f%% %8lu %5.3f%% %8lu %5.3f%%\n",CLSN[t],
                       ms_end[pi][t],100.0*ms_end[pi][t]/d,ms_traj[pi][t],100.0*ms_traj[pi][t]/d,
                       ms_cov[pi][t],100.0*ms_cov[pi][t]/d);
            }
        }
        printf("\n   Static disagreement with the reference path at a single pose\n");
        for(pi=0;pi<NPATH;++pi){
            printf("    %s\n",PATHN[pi]);
            for(t=0;t<CLS_N;++t){
                double d=(double)(cmp_runs[pi][t]?cmp_runs[pi][t]:1);
                printf("     %-14s %8lu %5.3f%% %8lu %5.3f%% %8lu %5.3f%%\n",CLSN[t],
                       st_end[pi][t],100.0*st_end[pi][t]/d,st_traj[pi][t],100.0*st_traj[pi][t]/d,
                       st_cov[pi][t],100.0*st_cov[pi][t]/d);
            }
        }
    }
    {
        int pi;
        printf("\n   MAGNITUDE of the approximation-induced transitions (not just incidence)\n");
        for(pi=0;pi<NPATH;++pi){
            printf("    %s\n",PATHN[pi]);
            printf("     %-14s %28s   %28s\n","step class",
                   "endpoint jump, screen px","cells added+removed");
            printf("     %-14s %8s %8s %8s   %8s %8s %8s\n","",
                   "<=1px","p95","max","<=2","p95","max");
            for(t=0;t<CLS_N;++t){
                const unsigned long *hp=mag_px[pi][t],*hc=mag_cells[pi][t];
                unsigned long tp=mag_total(hp),tc=mag_total(hc);
                printf("     %-14s %7.2f%% %8.0f %8.0f   %7.2f%% %8.0f %8.0f\n",CLSN[t],
                       tp?100.0*mag_atmost(hp,1)/(double)tp:0.0,mag_pct(hp,0.95),mag_pct(hp,1.0),
                       tc?100.0*mag_atmost(hc,2)/(double)tc:0.0,mag_pct(hc,0.95),mag_pct(hc,1.0));
            }
        }
        printf("     (a transition that moves an endpoint by one pixel or swaps one or two\n");
        printf("      cells is a threshold crossing arriving a step early or late; a large\n");
        printf("      jump would be a different and much worse result)\n");
    }

    printf("\n2  cost of a crossing\n");
    printf("   %lu coarse cells carry a baked record, %lu bytes total, %.1f B mean\n",
           loads,load_bytes,(double)load_bytes/(double)(loads?loads:1));
    printf("   a crossing re-parses one whole cell record; within a cell nothing is reloaded\n");
    return 0;
}
