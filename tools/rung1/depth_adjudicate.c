/* Rung 1.5 item 2: adjudicate both (iq,step) derivations against high-precision
 * perspective geometry, not against each other.
 *
 * The earlier layered check reported where the ROM's screen_depth_plane path and
 * the host's inv0/inv1 path DISAGREE. That says nothing about which is right.
 * This harness computes, in double precision from the wall plane and the pose,
 * what the renderer's own screen model says the answer is, and scores both
 * integer paths against it.
 *
 * The reference is not a second renderer. It is the closed geometry the fixed
 * point code is approximating. It lives in geometry_reference.h, shared with the
 * regression test that guards it; the derivation in brief:
 *
 *   bearing_q12 returns atan2 with the y term negated, so a world bearing b
 *   names the direction u(b) = (cos B, -sin B), B = 2*pi*b/4096.
 *   angle_x is round(80 + 80 tan theta) to within one LSB over its whole
 *   domain, so tile column c samples screen pixel x = 8c and
 *   tan theta = (8c - 80)/80. Column 10 is pixel 80, theta = 0, which is where
 *   screen_depth_plane anchors iq.
 *   k_tspf_invz[z] is round(2560/z) exactly for every entry, so the projection
 *   constant is K = 2560 and inverse depth is K/z with z the camera-axis depth.
 *   For a plane with unit normal n through V at perpendicular distance
 *   D = n.(V - P), the ray at theta meets it at t = D/(n.u(phi+theta)) and
 *   z = t cos theta, so
 *
 *     inv(x) = (K/D) * (A + B*(x-80)/80),  A = n.u(phi), B = -(nx sin phi + ny cos phi)
 *
 *   which is exactly linear in screen x: the depth-plane model is the correct
 *   model, and only its quantization is in question.
 *
 *   h = inv/2 pixels, y_top = 71 - h, y_bot = 72 + h.
 *
 * Every FULL wall in this map has a cardinal normal (nx,ny in {0,+-32} Q5), so n
 * is exact and D is exact: the reference carries no approximation of its own.
 *
 * Reported separately, by magnitude rather than incidence:
 *   per-column top/bottom Y error, in integer-pixel classes and as a distribution
 *   covered-cell symmetric difference against the reference, p50/p95/max
 *   each disputed cell classified boundary-adjacent / 1-cell-away / interior
 *   temporal monotonicity and reversal counts under smooth motion
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
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

static int in_domain(int16_t iq,int16_t step,int c){ int raw=(iq+c*step+32)>>6; return raw>=0&&raw<=255; }
static int hq(int16_t iq,int16_t step,int c){ return (int)((((iq+c*step+32)>>6)&0xFF)>>1); }
static int yq(int16_t iq,int16_t step,int c,int fam){ int h=hq(iq,step,c); return fam==0?71-h:72+h; }

/* ---- the reference -------------------------------------------------- */
/* The reference itself lives in geometry_reference.h, with the full derivation
 * and the provenance of every constant. It is shared verbatim with
 * tools/rung1/geometry_reference_test.c, which asserts the conventions against
 * the shipped tables, checks the closed linear form against explicit ray/plane
 * intersection, and checks hand-derived cases whose expected values come from
 * the geometry rather than from either implementation. That test is what stops
 * this result from becoming the next piece of folklore. */
#include "geometry_reference.h"

typedef GeomPlane Plane;

static void plane_of(uint8_t sid,uint8_t yaw,const TSPState *s,Plane *p)
{
    uint8_t av=k_tspf_seg_anchor[sid];
    geom_plane((double)k_tspf_nx_q5[sid]/32.0,(double)k_tspf_ny_q5[sid]/32.0,
               (double)k_tspf_vx[av],(double)k_tspf_vy[av],
               (double)s->x_q4/16.0,(double)s->y_q4/16.0,(double)yaw,p);
}
static double inv_true(const Plane *p,int c){ return geom_inv_at_column(p,c); }
static double inv_unclipped(const Plane *p,int c){ return geom_inv_unclipped(p,8.0*(double)c); }
static double y_true(const Plane *p,int c,int fam){ return geom_y_at_column(p,c,fam); }

/* ---- distributions --------------------------------------------------- */
#define EBINS 6401                 /* 0.01 px resolution up to 64 px */
typedef struct { unsigned long bin[EBINS]; unsigned long n; double sum,max; } Dist;
static void d_add(Dist *d,double e){
    int i; if(e<0) e=-e;
    i=(int)(e*100.0+0.5); if(i>=EBINS) i=EBINS-1;
    ++d->bin[i]; ++d->n; d->sum+=e; if(e>d->max) d->max=e;
}
static double d_pct(const Dist *d,double q){
    unsigned long want=(unsigned long)(q*(double)d->n), acc=0; int i;
    if(!d->n) return 0.0;
    for(i=0;i<EBINS;++i){ acc+=d->bin[i]; if(acc>=want) return (double)i/100.0; }
    return (double)(EBINS-1)/100.0;
}
static double d_mean(const Dist *d){ return d->n?d->sum/(double)d->n:0.0; }

/* integer-pixel agreement classes */
typedef struct { unsigned long exact,within1,within2,worse,n; } Cls;
static void c_add(Cls *c,int yi,double yt){
    int t=(int)floor(yt+0.5), d=yi-t; if(d<0) d=-d;
    ++c->n;
    if(d==0) ++c->exact; else if(d==1) ++c->within1; else if(d==2) ++c->within2; else ++c->worse;
}
static void c_print(const char *lbl,const Cls *c,const Dist *d){
    double n=(double)(c->n?c->n:1);
    printf("  %-22s exact %6.2f%%  1px %6.2f%%  2px %6.2f%%  >2px %6.2f%%   "
           "mean %5.2f  p95 %5.2f  max %6.2f  (n=%lu)\n",
           lbl,100.0*c->exact/n,100.0*c->within1/n,100.0*c->within2/n,100.0*c->worse/n,
           d_mean(d),d_pct(d,0.95),d->max,c->n);
}

/* ---- ownership ------------------------------------------------------- */
/* Returns 0 when the span lies entirely off the 18-row viewport, in which case
 * there is no ownership to compare and the caller must not treat the clamped
 * interval as a real one. */
static int rows_of(double ya,double yb,int *lo,int *hi){
    double a=ya<yb?ya:yb, b=ya<yb?yb:ya;
    int l=(int)floor(a/8.0), h=(int)floor(b/8.0);
    if(h<0||l>NT_ROWS-1) return 0;
    if(l<0) l=0;
    if(h>NT_ROWS-1) h=NT_ROWS-1;
    *lo=l; *hi=h; return 1;
}
/* classification counters */
static unsigned long cl_boundary[2],cl_one[2],cl_interior[2],cl_cells[2],cl_diff[2];
#define SDBINS 64
static unsigned long sdhist[2][SDBINS]; static unsigned long sdn[2]; static unsigned long sdmax[2];
/* how far the true edge sits from the tile boundary it is disputing */
static Dist bdist[2];

/* Classify every cell in the symmetric difference between the reference's covered
 * rows and an integer path's.
 *
 * Both are row intervals, so their symmetric difference always lies at the ends:
 * there is no such thing as a disputed cell strictly inside both. What varies is
 * HOW FAR the end is off, and how close the true edge sat to the tile line it was
 * being rounded against. Those are the two things that decide whether a
 * disagreement matters:
 *
 *   boundary-adjacent  the true edge is within 1px of the tile line, so which row
 *                      owns it is a coin flip no integer path can be blamed for
 *   1-cell-away        the end is off by exactly one row and the true edge was not
 *                      near the line: a real, smallest-possible error
 *   interior ownership the end is off by two or more rows: visibly wrong extent
 */
static void own_compare(int side,double ta,double tb,int ia,int ib)
{
    int tl,th,il,ih,r,has_i;
    double tlo=ta<tb?ta:tb, thi=ta<tb?tb:ta;
    double dlo,dhi;
    if(!rows_of(ta,tb,&tl,&th)) return;      /* reference span is off-screen */
    has_i=rows_of((double)ia,(double)ib,&il,&ih);
    if(!has_i){ il=1; ih=0; }                /* integer path draws nothing here */
    cl_cells[side]+=(unsigned long)(th-tl+1);
    if(tl==il&&th==ih) return;
    /* distance of each true edge from the nearest tile line it is rounded against */
    dlo=fabs(tlo-8.0*floor(tlo/8.0+0.5));
    dhi=fabs(thi-8.0*floor(thi/8.0+0.5));
    for(r=0;r<NT_ROWS;++r){
        int in_t=(r>=tl&&r<=th), in_i=(r>=il&&r<=ih), low, delta;
        double d;
        if(in_t==in_i) continue;
        ++cl_diff[side];
        /* which end of the interval this cell belongs to */
        low = (r < (tl<il?il:tl));
        d      = low ? dlo : dhi;
        delta  = low ? (tl-il) : (ih-th);
        if(delta<0) delta=-delta;
        d_add(&bdist[side],d);
        if(d<1.0) ++cl_boundary[side];
        else if(delta<=1) ++cl_one[side];
        else ++cl_interior[side];
    }
}
static void sd_record(int side,int sd){
    if(sd>=SDBINS) sd=SDBINS-1;
    ++sdhist[side][sd]; ++sdn[side];
    if((unsigned long)sd>sdmax[side]) sdmax[side]=(unsigned long)sd;
}
static double sd_pct(int side,double q){
    unsigned long want=(unsigned long)(q*(double)sdn[side]),acc=0; int i;
    if(!sdn[side]) return 0.0;
    for(i=0;i<SDBINS;++i){ acc+=sdhist[side][i]; if(acc>=want) return (double)i; }
    return SDBINS-1;
}

static Cls cls_top[2],cls_bot[2];
static Dist err_top[2],err_bot[2];
static unsigned long g_runs_n=0,g_cols=0;

/* one run's worth of scoring; side 0 = ROM depth plane, 1 = host inv0/inv1.
 *
 * Both paths are scored on EXACTLY the same columns. A column is comparable only
 * when both integer derivations stay inside the uint8 depth domain and the
 * reference itself is representable in it. The excluded cases are counted and
 * reported rather than folded into the error, because they are not depth-plane
 * errors: an inverse depth above 255 is outside what an 8-bit depth term can
 * express at all, and invd itself saturates at TSPF_NEAR_Z_Q4 for both paths.
 */
static unsigned long g_ex_rom=0,g_ex_host=0,g_ex_near=0;
static unsigned long g_clip_near=0,g_clip_far=0;
static Dist clip_err;   /* |y under the renderer's clip - y under unclipped geometry| */

static int comparable(int16_t iqa,int16_t sa,int16_t iqb,int16_t sb,const Plane *pl,int c0,int c)
{
    int ok=1;
    if(!in_domain(iqa,sa,c)){ ++g_ex_rom; ok=0; }
    if(!in_domain(iqb,sb,c)){ ++g_ex_host; ok=0; }
    if(inv_true(pl,c0+c)>255.5){ ++g_ex_near; ok=0; }   /* beyond an 8-bit depth term entirely */
    return ok;
}
static void score_side(int side,int16_t iq,int16_t step,int c0,int n,const Plane *pl,const char *ok)
{
    int c,sd_top=0,sd_bot=0,pairs=0;
    for(c=0;c<n;++c){
        if(!ok[c]) continue;
        if(side==0){
            ++g_cols;
            if(pl->clipped){
                double hc=inv_true(pl,c0+c)*0.5, hu=inv_unclipped(pl,c0+c)*0.5;
                if(hu>127.5) hu=127.5;
                if(pl->clipped==1) ++g_clip_near; else ++g_clip_far;
                d_add(&clip_err,hc-hu);
            }
        }
        c_add(&cls_top[side],yq(iq,step,c,0),y_true(pl,c0+c,0));
        c_add(&cls_bot[side],yq(iq,step,c,2),y_true(pl,c0+c,2));
        d_add(&err_top[side],(double)yq(iq,step,c,0)-y_true(pl,c0+c,0));
        d_add(&err_bot[side],(double)yq(iq,step,c,2)-y_true(pl,c0+c,2));
    }
    for(c=0;c+1<n;++c){
        unsigned long b0;
        if(!ok[c]||!ok[c+1]) continue;
        ++pairs;
        b0=cl_diff[side];
        own_compare(side,y_true(pl,c0+c,0),y_true(pl,c0+c+1,0),yq(iq,step,c,0),yq(iq,step,c+1,0));
        sd_top+=(int)(cl_diff[side]-b0);
        b0=cl_diff[side];
        own_compare(side,y_true(pl,c0+c,2),y_true(pl,c0+c+1,2),yq(iq,step,c,2),yq(iq,step,c+1,2));
        sd_bot+=(int)(cl_diff[side]-b0);
    }
    if(pairs){ sd_record(side,sd_top); sd_record(side,sd_bot); }
}

/* ---- pose enumeration ------------------------------------------------ */
typedef void (*run_fn)(const TSPState *s,uint8_t yaw,PolarRun *r,int cc0,int cc1,
                       int16_t iq_rom,int16_t st_rom,int16_t iq_host,int16_t st_host,void *ud);

static void for_each_full_run(const TSPState *s,unsigned gx,unsigned gy,unsigned yaw,run_fn fn,void *ud)
{
    uint8_t ks[64],nk=0,count=0,j;
    uint8_t recipe,base_id,cond_count,lx,ly; uint16_t gi,offs; unsigned i,c;
    const uint8_t *p,*b;
    gi=(uint16_t)(((uint16_t)gy<<5)+((uint16_t)gy<<4)+gx);
    recipe=k_tspf_recipe_grid[gi]; if(recipe==0xffu) return;
    lx=(uint8_t)((uint16_t)s->x_q4&63u); ly=(uint8_t)((uint16_t)s->y_q4&63u);
    offs=k_tspf_recipe_off[recipe]; p=&k_tspf_recipe_stream[offs];
    base_id=*p++; cond_count=*p++;
    b=&k_tspf_base_stream[k_tspf_base_off[base_id]]; i=*b++;
    for(;i;--i) ks[nk++]=*b++;
    for(i=0;i<cond_count;++i){ uint8_t key=*p++,sel=*p++; if(selector_pass(sel,lx,ly)) ks[nk++]=key; }
    tsp_polar_renderer_reset(); g_corner_bearing_valid=0u;
    for(j=0;j<nk;++j){ if(count>=TSPF_MAX_ACTIVE) break;
        if(!project_key(ks[j],(TSPState*)s,&g_runs[count])) continue; insert_run(count,&count); }
    for(c=0;c<count;++c){
        PolarRun *r=&g_runs[c];
        uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),n,invd;
        int16_t iq_a,st_a,iq_b,st_b;
        if(cc0>=TSP_COLS) cc0=TSP_COLS-1; if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
        if(cc1<cc0) continue;
        if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
        n=(uint8_t)(cc1-cc0+1u);
        iq_a=(int16_t)((int16_t)r->inv0<<6);
        st_a=shr_signed((int16_t)(((int16_t)r->inv1-(int16_t)r->inv0)*(int16_t)k_col_recip_q8[n]),2);
        invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],(TSPState*)s));
        if(!dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&iq_b,&st_b)) continue;
        fn(s,(uint8_t)yaw,r,cc0,cc1,iq_b,st_b,iq_a,st_a,ud);
    }
}

static void sweep_cb(const TSPState *s,uint8_t yaw,PolarRun *r,int cc0,int cc1,
                     int16_t iq_rom,int16_t st_rom,int16_t iq_host,int16_t st_host,void *ud)
{
    Plane pl; (void)ud;
    char ok[32]; int c,n=cc1-cc0+1,any=0;
    plane_of(r->sid,yaw,s,&pl);
    if(pl.D>-0.0001&&pl.D<0.0001) return;
    if(n>32) n=32;
    for(c=0;c<n;++c){ ok[c]=(char)comparable(iq_rom,st_rom,iq_host,st_host,&pl,cc0,c); any|=ok[c]; }
    if(!any) return;
    ++g_runs_n;
    score_side(0,iq_rom,st_rom,cc0,n,&pl,ok);
    score_side(1,iq_host,st_host,cc0,n,&pl,ok);
}

/* ---- temporal jitter -------------------------------------------------- */
/* A sequence is "monotone" if it never reverses direction. Reversals are counted
 * on the strictly-changing subsequence, so a plateau is not a reversal. */
static int reversals(const int *v,int n){
    int i,dir=0,rev=0;
    for(i=1;i<n;++i){ int d=v[i]-v[i-1]; if(!d) continue;
        if(dir&&((d>0)!=(dir>0))) ++rev; dir=d; }
    return rev;
}
static int reversals_d(const double *v,int n){
    /* dir must be a double: truncating a sub-pixel step to int makes it zero and
     * the reversal test then never fires, which reports every sequence monotone. */
    int i,rev=0; double dir=0.0;
    for(i=1;i<n;++i){ double d=v[i]-v[i-1]; if(d>-1e-9&&d<1e-9) continue;
        if(dir!=0.0&&((d>0.0)!=(dir>0.0))) ++rev; dir=d; }
    return rev;
}

typedef struct { int sid,col,have; int yr[512],yh[512],yt[512]; double ytf[512]; int n; } Track;
static Track g_tr;
static void track_cb(const TSPState *s,uint8_t yaw,PolarRun *r,int cc0,int cc1,
                     int16_t iq_rom,int16_t st_rom,int16_t iq_host,int16_t st_host,void *ud)
{
    Plane pl; int c; (void)ud;
    /* One sample per step. A single sid can be reached through more than one key,
     * so without this the same step contributes twice and manufactures a reversal
     * that the renderer never produced. */
    if(g_tr.have) return;
    if(r->sid!=g_tr.sid) return;
    if(g_tr.col<cc0||g_tr.col>cc1) return;
    c=g_tr.col-cc0;
    if(!in_domain(iq_rom,st_rom,c)||!in_domain(iq_host,st_host,c)) return;
    plane_of(r->sid,yaw,s,&pl);
    if(g_tr.n<512){
        g_tr.yr[g_tr.n]=yq(iq_rom,st_rom,c,0);
        g_tr.yh[g_tr.n]=yq(iq_host,st_host,c,0);
        g_tr.ytf[g_tr.n]=y_true(&pl,g_tr.col,0);
        g_tr.yt[g_tr.n]=(int)floor(g_tr.ytf[g_tr.n]+0.5);
        ++g_tr.n;
    }
    g_tr.have=1;
}

/* Three smooth motions, each one renderer-input unit per step: strafe along +x,
 * walk along the facing direction, and turn. A sequence is kept only while the
 * same wall still covers the same screen column, so the samples really are a
 * continuous view of one edge. */
#define JMODES 3
static unsigned long jit_seq[JMODES],jit_steps[JMODES];
static unsigned long jit_rev_rom[JMODES],jit_rev_host[JMODES],jit_rev_true[JMODES];
static unsigned long jit_mono_rom[JMODES],jit_mono_host[JMODES],jit_mono_true[JMODES];
static unsigned long jit_seq_total=0;
static FILE *g_jit=NULL;

static void jitter_probe(unsigned gx,unsigned gy,int sid,int col,int mode,unsigned yaw0,
                         int nsteps,FILE *out)
{
    TSPState s; int k,rr,rh,rt;
    double fx=cos(2.0*M_PI*(double)yaw0/256.0), fy=-sin(2.0*M_PI*(double)yaw0/256.0);
    memset(&s,0,sizeof s);
    g_tr.sid=sid; g_tr.col=col; g_tr.n=0;
    for(k=0;k<nsteps;++k){
        unsigned yaw=yaw0;
        int16_t bx=(int16_t)(gx*CELL_Q4+32),by=(int16_t)(gy*CELL_Q4+32);
        if(mode==0){ s.x_q4=(int16_t)(bx+k); s.y_q4=by; }
        else if(mode==1){ s.x_q4=(int16_t)(bx+(int)(fx*(double)k)); s.y_q4=(int16_t)(by+(int)(fy*(double)k)); }
        else { s.x_q4=bx; s.y_q4=by; yaw=(yaw0+(unsigned)k)&255u; }
        s.yaw=(uint8_t)yaw;
        if(!tsp_is_walkable_q4(s.x_q4,s.y_q4)) break;
        g_tr.have=0;
        for_each_full_run(&s,(unsigned)(s.x_q4/CELL_Q4),(unsigned)(s.y_q4/CELL_Q4),yaw,track_cb,NULL);
        if(!g_tr.have) break;
    }
    if(g_tr.n<12) return;
    rr=reversals(g_tr.yr,g_tr.n); rh=reversals(g_tr.yh,g_tr.n); rt=reversals(g_tr.yt,g_tr.n);
    ++jit_seq[mode]; ++jit_seq_total; jit_steps[mode]+=(unsigned long)g_tr.n;
    jit_rev_rom[mode]+=(unsigned long)rr; jit_rev_host[mode]+=(unsigned long)rh;
    jit_rev_true[mode]+=(unsigned long)rt;
    if(rr<=rt) ++jit_mono_rom[mode];
    if(rh<=rt) ++jit_mono_host[mode];
    if(reversals_d(g_tr.ytf,g_tr.n)==0) ++jit_mono_true[mode];
    if(out&&jit_seq_total<=4000){ for(k=0;k<g_tr.n;++k)
        fprintf(out,"%lu,%d,%d,%d,%d,%d,%.3f\n",jit_seq_total,mode,k,
                g_tr.yr[k],g_tr.yh[k],g_tr.yt[k],g_tr.ytf[k]); }
}

int main(int argc,char**argv)
{
    unsigned yaw_step=argc>1?(unsigned)strtoul(argv[1],0,0):1u;
    unsigned cell_stride=argc>2?(unsigned)strtoul(argv[2],0,0):4u;
    unsigned sub=argc>3?(unsigned)strtoul(argv[3],0,0):8u;   /* sub-cell grid: sub x sub */
    const char *od=argc>4?argv[4]:NULL;
    TSPState s; unsigned gx,gy,yaw,ox,oy,cellno=0;
    int side;
    char pth[512];

    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t px0=(int16_t)(gx*CELL_Q4+32),py0=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(px0,py0)) continue;
        ++cellno; if((cellno-1u)%cell_stride) continue;
        for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
            int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub)),py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
            if(!tsp_is_walkable_q4(px,py)) continue;
            for(yaw=0;yaw<256u;yaw+=yaw_step){
                memset(&s,0,sizeof s); s.x_q4=px; s.y_q4=py; s.yaw=(uint8_t)yaw;
                for_each_full_run(&s,gx,gy,yaw,sweep_cb,NULL);
            }
        }
    }

    printf("adjudication against high-precision perspective geometry\n");
    printf("  poses: every %uth walkable cell x %ux%u sub-cell x %u headings\n",
           cell_stride,sub,sub,256u/yaw_step);
    printf("  FULL runs scored %lu, columns scored %lu (both paths, identical column set)\n",
           g_runs_n,g_cols);
    printf("  columns excluded: ROM outside uint8 depth domain %lu, host outside %lu,\n"
           "                    reference inverse depth above 255 (near field, unrepresentable\n"
           "                    in an 8-bit depth term by either path) %lu\n\n",
           g_ex_rom,g_ex_host,g_ex_near);

    printf("renderer near/far depth clip, shared by both derivations\n");
    printf("  columns behind the 10-cell near clip %lu (%.2f%%), past the 127-cell far clip %lu (%.2f%%)\n",
           g_clip_near,100.0*g_clip_near/(double)(g_cols?g_cols:1),
           g_clip_far,100.0*g_clip_far/(double)(g_cols?g_cols:1));
    printf("  edge displacement the clip alone causes: mean %.2f  p95 %.2f  max %.2f px\n",
           d_mean(&clip_err),d_pct(&clip_err,0.95),clip_err.max);
    printf("  (charged to neither derivation below: the reference carries the same clip)\n\n");

    printf("per-column screen Y against the reference\n");
    for(side=0;side<2;++side){
        printf(" %s\n",side==0?"ROM screen_depth_plane":"host inv0/inv1 endpoint interpolation");
        c_print("top edge  y=71-h",&cls_top[side],&err_top[side]);
        c_print("bottom edge y=72+h",&cls_bot[side],&err_bot[side]);
    }

    printf("\ncovered-cell ownership against the reference\n");
    for(side=0;side<2;++side){
        double n=(double)(cl_diff[side]?cl_diff[side]:1);
        printf(" %s\n",side==0?"ROM screen_depth_plane":"host inv0/inv1 endpoint interpolation");
        printf("  reference cells owned      %lu\n",cl_cells[side]);
        printf("  symmetric difference       %lu cells (%.3f%% of reference cells)\n",
               cl_diff[side],100.0*(double)cl_diff[side]/(double)(cl_cells[side]?cl_cells[side]:1));
        printf("  per-run sym-diff p50/p95/max  %.0f / %.0f / %lu cells\n",
               sd_pct(side,0.50),sd_pct(side,0.95),sdmax[side]);
        printf("  of the disputed cells:\n");
        printf("    boundary-adjacent        %9lu (%6.2f%%)  true edge within 1px of its tile line: a coin flip\n",
               cl_boundary[side],100.0*cl_boundary[side]/n);
        printf("    1-cell-away              %9lu (%6.2f%%)  end off by exactly one row, line not close\n",
               cl_one[side],100.0*cl_one[side]/n);
        printf("    interior ownership       %9lu (%6.2f%%)  end off by two or more rows\n",
               cl_interior[side],100.0*cl_interior[side]/n);
        printf("  distance of disputed edge from its tile boundary: mean %.2f  p50 %.2f  p95 %.2f  max %.2f px\n",
               d_mean(&bdist[side]),d_pct(&bdist[side],0.50),d_pct(&bdist[side],0.95),bdist[side].max);
    }

    if(od){ snprintf(pth,sizeof pth,"%s/jitter.csv",od); g_jit=fopen(pth,"w");
            if(g_jit) fprintf(g_jit,"seq,mode,step,y_rom,y_host,y_true_int,y_true\n"); }
    {
        unsigned sid,mode,n=0,col,y0;
        for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
            int16_t px0=(int16_t)(gx*CELL_Q4+32),py0=(int16_t)(gy*CELL_Q4+32);
            if(!tsp_is_walkable_q4(px0,py0)) continue;
            ++n; if((n-1u)%2u) continue;
            for(sid=0;sid<17u;++sid){ if(k_tspf_profile[sid]!=TSP_PROFILE_FULL) continue;
                for(col=2u;col<=18u;col+=4u)
                    for(y0=0;y0<256u;y0+=16u)
                        for(mode=0;mode<JMODES;++mode)
                            jitter_probe(gx,gy,(int)sid,(int)col,(int)mode,y0,mode==2?128:48,g_jit); } }
    }
    if(g_jit) fclose(g_jit);
    printf("\ntemporal behaviour under smooth motion (one input unit per step)\n");
    printf("  %-12s %8s %10s   reversals per 100 samples        sequences no worse than reference\n",
           "motion","seqs","samples");
    {
        static const char *mn[JMODES]={"strafe","walk forward","turn"};
        unsigned long tseq=0,tstep=0,tr_=0,tro=0,trh=0,tmo=0,tmh=0,tmt=0;
        int m;
        for(m=0;m<JMODES;++m){
            if(!jit_seq[m]) continue;
            printf("  %-12s %8lu %10lu   ref %5.2f  ROM %5.2f  host %5.2f   ROM %5.1f%%  host %5.1f%%\n",
                   mn[m],jit_seq[m],jit_steps[m],
                   100.0*jit_rev_true[m]/(double)jit_steps[m],
                   100.0*jit_rev_rom[m]/(double)jit_steps[m],
                   100.0*jit_rev_host[m]/(double)jit_steps[m],
                   100.0*jit_mono_rom[m]/(double)jit_seq[m],
                   100.0*jit_mono_host[m]/(double)jit_seq[m]);
            tseq+=jit_seq[m]; tstep+=jit_steps[m]; tr_+=jit_rev_true[m]; tro+=jit_rev_rom[m];
            trh+=jit_rev_host[m]; tmo+=jit_mono_rom[m]; tmh+=jit_mono_host[m]; tmt+=jit_mono_true[m];
        }
        if(tseq) printf("  %-12s %8lu %10lu   ref %5.2f  ROM %5.2f  host %5.2f   ROM %5.1f%%  host %5.1f%%\n",
                        "ALL",tseq,tstep,100.0*tr_/(double)tstep,100.0*tro/(double)tstep,
                        100.0*trh/(double)tstep,100.0*tmo/(double)tseq,100.0*tmh/(double)tseq);
        if(tseq) printf("  the continuous reference is itself monotone in %lu/%lu sequences (%.1f%%);\n"
                        "  a reversal there is real geometry, not quantization\n",
                        tmt,tseq,100.0*tmt/(double)tseq);
    }

    if(od){
        FILE *f; int i;
        snprintf(pth,sizeof pth,"%s/agree_classes.csv",od); f=fopen(pth,"w");
        if(f){ fprintf(f,"path,edge,exact,px1,px2,worse\n");
               for(side=0;side<2;++side){
                   fprintf(f,"%s,top,%lu,%lu,%lu,%lu\n",side?"host":"rom",
                           cls_top[side].exact,cls_top[side].within1,cls_top[side].within2,cls_top[side].worse);
                   fprintf(f,"%s,bottom,%lu,%lu,%lu,%lu\n",side?"host":"rom",
                           cls_bot[side].exact,cls_bot[side].within1,cls_bot[side].within2,cls_bot[side].worse); }
               fclose(f); }
        snprintf(pth,sizeof pth,"%s/boundary_dist.csv",od); f=fopen(pth,"w");
        if(f){ fprintf(f,"path,dist_px,count\n");
               for(side=0;side<2;++side) for(i=0;i<EBINS;++i) if(bdist[side].bin[i])
                   fprintf(f,"%s,%.2f,%lu\n",side?"host":"rom",(double)i/100.0,bdist[side].bin[i]);
               fclose(f); }
        snprintf(pth,sizeof pth,"%s/ownership.csv",od); f=fopen(pth,"w");
        if(f){ fprintf(f,"path,boundary_adjacent,one_cell_away,interior,reference_cells\n");
               for(side=0;side<2;++side)
                   fprintf(f,"%s,%lu,%lu,%lu,%lu\n",side?"host":"rom",
                           cl_boundary[side],cl_one[side],cl_interior[side],cl_cells[side]);
               fclose(f); }
    }
    return 0;
}
