/* Rung 3: does the analytic phase-band certificate actually predict temporal
 * stability under real player motion?
 *
 * The band result says a 6-column raster behaviour is constant while the DDA
 * phase stays inside its band, and that the band edges are exactly
 * p = (-c*step) mod 1024 for c in 0..7. That is a statement in phase space. It
 * is not yet a statement about a player walking through a map, because moving
 * the player changes more than the phase: it changes step, the projected
 * endpoints, and therefore the span length.
 *
 * So this is a falsification test, not a census. For every retained span it
 * computes what the certificate CLAIMS is safe, then moves the player one unit
 * at a time and records when the emitted raster actually changes.
 *
 *   certificate SAFE at step k  <=>  step unchanged
 *                                AND projected columns c0,c1 unchanged
 *                                AND no band edge crossed by any chunk's phase
 *
 * The property that must hold is one-sided: the certificate may be conservative
 * (claim unsafe while the output is in fact still identical) but it must NEVER
 * claim safe while the output has changed. Any such case is an UNSAFE VIOLATION
 * and the mechanism is wrong.
 *
 * Every real change is also attributed to a cause, so that what remains after
 * phase certificates is visible: if endpoint or length events dominate, that is
 * where the next work belongs, not in the phase machinery.
 *
 * Two bearing sources are measured. Continuous-truth bearings validate the
 * certificate mechanism itself; bearing_q12 shows what the ratio_q8_exact defect
 * does to retained state, which is the case for fixing it before any temporal
 * integration.
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
#define CHUNK 6
#define NT_ROWS 18
#define MAXRUN 24
#define KSTEPS 32
#define COVW 6

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

static void cov_set(uint64_t *c,int row,int col)
{ int b; if(row<0||row>=NT_ROWS||col<0||col>=20) return; b=row*20+col; c[b>>6]|=1ull<<(b&63); }
static int cov_eq(const uint64_t *a,const uint64_t *b)
{ int i; for(i=0;i<COVW;++i) if(a[i]!=b[i]) return 0; return 1; }

typedef struct {
    uint8_t sid,c0,c1,ok,live;
    int16_t iq,step;
    uint64_t traj;      /* the emitted move stream */
    uint64_t prog;      /* the 6-column BEHAVIOUR of each chunk, rows excluded */
    uint64_t rows;      /* each chunk's absolute start row */
    uint64_t band;      /* each chunk's band index */
    uint64_t cover[COVW];
} Span;

/* The 6-column behaviour of one chunk: the (ndown, jump) pair per column, over
 * both families, with no absolute row in it. Two states with the same signature
 * emit the same PROGRAM even if the whole span sits a row higher or lower, and
 * even if the numeric step differs. This is the invariant the exact-step test
 * was standing in for, and it is the one rotation needs. */
static uint64_t chunk_sig6(int16_t iq,int16_t step)
{
    uint64_t h=1469598103934665603ull; int fam,c;
    for(fam=0;fam<2;++fam) for(c=0;c<6;++c){
        long a0=(long)iq+32+(long)c*step, a1=a0+step, a2=a1+step;
        long h0=a0>>7,h1=a1>>7,h2=a2>>7;
        long y0=fam?72+h0:71-h0,y1=fam?72+h1:71-h1,y2=fam?72+h2:71-h2;
        long lo0=(y0<y1?y0:y1)>>3,hi0=(y0<y1?y1:y0)>>3;
        long lo1=(y1<y2?y1:y2)>>3;
        h^=(uint64_t)((hi0-lo0)*8+(hi0-lo1)+64); h*=1099511628211ull;
    }
    return h;
}
typedef struct { int n; Span s[MAXRUN]; } Frame;

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
        for(k=0;k<hi[c]-lo[c];++k){ *traj^=39u; *traj*=1099511628211ull; cov_set(cover,row,c0abs+col); ++row; }
        cov_set(cover,row,c0abs+col);
        if(is_last&&c==want-1){ *traj^=200u; *traj*=1099511628211ull; break; }
        jump=lo[c+1]-hi[c];
        if(jump>0||jump<-4){ *ok=0; return; }
        *traj^=(uint64_t)(100+jump); *traj*=1099511628211ull;
        ++col; row+=jump;
    }
}

/* ---- the certificate ----------------------------------------------------- */
/* Band edges for this step, as phases. A behaviour change can only happen where
 * p + c*step == 0 (mod 1024) for c in 0..7, so the safe interval around the
 * current phase is bounded by the nearest such edge in the direction travelled.
 * The predicted set is a superset of the real edges, which is the safe
 * direction: it can wake us early, never late. */
static void band_edges(int16_t step,int *e)
{
    int c;
    for(c=0;c<8;++c){ long v=-(long)c*(long)step; v%=1024L; if(v<0) v+=1024L; e[c]=(int)v; }
}
/* Did the phase, moving from p0 by signed delta d, pass any edge?
 *
 * An edge at e separates the band starting at e from the one below it. Crossing
 * is therefore ASYMMETRIC, and getting that wrong is not cosmetic: with step 0
 * all eight edges collapse onto phase 0, and a span sitting exactly on that edge
 * and moving backwards shifts a whole row while a symmetric test reports no
 * crossing. That produced every one of the unsafe cases in the first run.
 *
 *   forward  by m:  positions p+1 .. p+m are entered, so e is crossed when
 *                   (e - p) mod 1024 lies in [1, m]
 *   backward by m:  the move e -> e-1 is what crosses e, so e is crossed when
 *                   (p - e) mod 1024 lies in [0, m-1]   -- note 0 counts
 */
static int crossed(int p0,long d,const int *e)
{
    int c; long m = d<0 ? -d : d;
    if(d==0) return 0;
    if(m>=1024) return 1;
    for(c=0;c<8;++c){
        if(d>0){ long g=(long)((e[c]-p0)&1023); if(g>=1&&g<=m) return 1; }
        else   { long g=(long)((p0-e[c])&1023); if(g<=m-1)     return 1; }
    }
    return 0;
}
static int phase_of(int16_t iq){ long v=((long)iq+32L)%1024L; if(v<0) v+=1024; return (int)v; }
/* unwrapped signed phase delta between two accumulator values */
static long phase_delta(int16_t iq0,int16_t iq1){ return ((long)iq1+32L)-((long)iq0+32L); }

/* margin: smallest |distance| from the current chunk phases to any edge */
/* Largest |phase movement| that provably crosses no edge, in either direction.
 * Forward is safe for (e-p)-1 steps, backward for (p-e) steps, matching crossed(). */
static int margin_phase(int16_t iq,int16_t step,int nchunks,const int *e)
{
    int best=1024,j,c;
    for(j=0;j<nchunks;++j){
        int p=phase_of((int16_t)(iq+(int16_t)(j*CHUNK*step)));
        for(c=0;c<8;++c){
            int f=((e[c]-p)&1023)-1;      /* forward steps before entering e */
            int b=((p-e[c])&1023);        /* backward steps before leaving e */
            if(f<0) f=1023;
            if(f<best) best=f;
            if(b<best) best=b;
        }
    }
    return best;
}

#define PATH_INT 0
#define PATH_TRUE 1
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
static void sample(int16_t px,int16_t py,unsigned yaw,int path,Frame *f)
{
    TSPState s; uint8_t ks[64],nk=0,count=0,j;
    uint8_t recipe,base_id,cond_count,lx,ly; uint16_t gi,offs; unsigned i,c,gx,gy;
    const uint8_t *p,*b;
    memset(f,0,sizeof *f);
    gx=(unsigned)(px/CELL_Q4); gy=(unsigned)(py/CELL_Q4);
    if(gx>=GRID_W||gy>=GRID_H) return;
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
    if(path==PATH_TRUE){ unsigned v;
        for(v=0;v<14u;++v){ g_corner_bearing_q12[v]=true_bearing_q12(v,px,py);
                            g_corner_bearing_valid|=k_corner_mask[v]; } }
    for(j=0;j<nk;++j){ if(count>=TSPF_MAX_ACTIVE) break;
        if(!project_key(ks[j],&s,&g_runs[count])) continue;
        insert_run(count,&count); }
    for(c=0;c<count&&f->n<MAXRUN;++c){
        PolarRun *r=&g_runs[c];
        uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),n,invd;
        int16_t iq,step; int fam,left,cs; Span *o;
        if(cc0>=TSP_COLS) cc0=TSP_COLS-1;
        if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
        if(cc1<cc0) continue;
        if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
        n=(uint8_t)(cc1-cc0+1u);
        invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],&s));
        o=&f->s[f->n++];
        memset(o,0,sizeof *o);
        o->sid=r->sid; o->c0=cc0; o->c1=cc1; o->live=1; o->ok=1;
        o->traj=1469598103934665603ull;
        if(!dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&iq,&step)){ o->ok=0; continue; }
        o->iq=iq; o->step=step;
        o->prog=1469598103934665603ull; o->rows=1469598103934665603ull;
        o->band=1469598103934665603ull;
        {   int nch=((int)n+CHUNK-1)/CHUNK,j,e2[8];
            band_edges(step,e2);
            for(j=0;j<nch;++j){
                int16_t q=(int16_t)(iq+(int16_t)(j*CHUNK*step));
                int ph=phase_of(q),bi=0,c2;
                o->prog^=chunk_sig6(q,step); o->prog*=1099511628211ull;
                o->rows^=(uint64_t)(((71-(((long)q+32)>>7))>>3)+256); o->rows*=1099511628211ull;
                /* band index: how many edges lie within half a turn behind this
                 * phase. Coincident edges (step 0 collapses all eight onto 0)
                 * simply contribute together, so the degenerate topology is
                 * represented rather than special-cased. */
                for(c2=0;c2<8;++c2) if(((ph-e2[c2])&1023)<512) ++bi;
                o->band^=(uint64_t)(bi+1); o->band*=1099511628211ull;
            } }
        for(fam=0;fam<=2;fam+=2){
            int okf=1; left=n; cs=0;
            while(left>0&&okf){
                int want=left>CHUNK?CHUNK:left;
                int16_t q=(int16_t)(iq+(int16_t)(cs*step));
                chunk_state(q,step,fam,want,left-want==0,cc0+cs,&o->traj,o->cover,&okf);
                cs+=CHUNK; left-=want;
            }
            if(!okf) o->ok=0;
        }
    }
}
static Span *find(Frame *f,uint8_t sid)
{ int i; for(i=0;i<f->n;++i) if(f->s[i].sid==sid) return &f->s[i]; return 0; }

enum { M_X=0, M_Y=1, M_YAW=2, M_N=3 };
static const char *MN[M_N]={"+X (1/16 cell)","+Y (1/16 cell)","+yaw (1 unit)"};
enum { C_PHASE=0, C_STEP=1, C_ENDPOINT=2, C_LOST=3, C_NONE=4, C_NOCHANGE=5, C_N=6 };
static const char *CN[C_N]={"phase band","step","endpoint column","left frame",
                            "UNATTRIBUTED","no change in 32 steps"};

/* Three certificates, measured on the same corpus so they are directly
 * comparable.
 *
 *   STRICT   step equal, columns equal, no band edge crossed. Full reuse: the
 *            span needs no work at all. This is what Rung 3 first measured, and
 *            it was designed with translation in mind.
 *   PROGRAM  the 6-column BEHAVIOUR of every chunk is unchanged and the columns
 *            are unchanged, but the numeric step and the absolute rows may move.
 *            The compiled move program is reusable; at most its start row needs
 *            adjusting. This is behavioural-region stability.
 *   CEILING  the emitted raster is literally unchanged. Not a certificate -- it
 *            is the answer -- but it bounds what any sound certificate could
 *            achieve on this state, so the gap to it is the headroom.
 */
/*   SHAPE    the 6-column behaviour of every chunk is unchanged, but the
 *            columns and the absolute rows may both move. The compiled move
 *            program is reusable as a program; where it is drawn may differ.
 *            This isolates whether rotation preserves the raster shape and
 *            merely slides it across the screen.
 *
 * Each tier certifies a different thing, so each has its own soundness test:
 * STRICT and PROGRAM are checked against the quantity they claim, not against
 * raster equality, which only STRICT implies.
 */
#define CERT_N 4
static const char *CERTN[CERT_N]={"STRICT  (nothing to do)","PROGRAM (program + columns)",
                                  "SHAPE   (program only)","CEILING (raster unchanged)"};
static const char *CERTW[CERT_N]={"raster unchanged","program+columns unchanged",
                                  "program unchanged","-"};
static unsigned long certhist[2][M_N][CERT_N][KSTEPS+2];
static unsigned long spans[2][M_N],viol[2][M_N],viol_c[2][M_N][CERT_N];
static unsigned long cause[2][M_N][C_N];
static unsigned long predhist[2][M_N][KSTEPS+2],obshist[2][M_N][KSTEPS+2];
static unsigned long conserv[2][M_N],exactpred[2][M_N];
static unsigned long marginhist[2][KSTEPS+2];
static FILE *g_csv=NULL;
static int g_dbg=0;
static unsigned long g_row=0;   /* the CSV is a 1-in-64 sample; the statistics above are not */

int main(int argc,char**argv)
{
    unsigned cell_stride=argc>1?(unsigned)strtoul(argv[1],0,0):8u;
    unsigned sub=argc>2?(unsigned)strtoul(argv[2],0,0):4u;
    unsigned yaw_step=argc>3?(unsigned)strtoul(argv[3],0,0):16u;
    const char *od=argc>4?argv[4]:NULL;
    unsigned gx,gy,yaw,ox,oy,cellno=0; int path,mode;
    char pth[512];

    if(od){ snprintf(pth,sizeof pth,"%s/certificate.csv",od); g_csv=fopen(pth,"w");
            if(g_csv) fprintf(g_csv,"path,mode,sid,phase,step,ncol,margin,pred_first,obs_first,cause\n"); }

    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(cx,cy)) continue;
        ++cellno; if((cellno-1u)%cell_stride) continue;
        for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
            int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
            int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
            if(!tsp_is_walkable_q4(px,py)) continue;
            for(yaw=0;yaw<256u;yaw+=yaw_step) for(path=0;path<2;++path){
                Frame f0; int i;
                sample(px,py,yaw,path,&f0);
                for(i=0;i<f0.n;++i){
                    Span *s0=&f0.s[i];
                    int e[8],nch,marg;
                    if(!s0->ok) continue;
                    band_edges(s0->step,e);
                    nch=((int)(s0->c1-s0->c0+1)+CHUNK-1)/CHUNK;
                    marg=margin_phase(s0->iq,s0->step,nch,e);
                    for(mode=0;mode<M_N;++mode){
                        int k,pred=-1,obs=-1,why=C_NOCHANGE,predp=-1,preds=-1;
                        int firstp=-1,firsts=-1;
                        for(k=1;k<=KSTEPS;++k){
                            Frame fk; Span *sk; int safe=1,j;
                            int16_t qx=px,qy=py; unsigned qyaw=yaw;
                            if(mode==M_X) qx=(int16_t)(px+k);
                            else if(mode==M_Y) qy=(int16_t)(py+k);
                            else qyaw=(yaw+(unsigned)k)&255u;
                            if(!tsp_is_walkable_q4(qx,qy)) break;
                            sample(qx,qy,qyaw,path,&fk);
                            sk=find(&fk,s0->sid);
                            if(!sk||!sk->ok||!sk->live){ if(obs<0){ obs=k; why=C_LOST; }
                                                         if(pred<0) pred=k;
                                                         break; }
                            /* certificate */
                            if(sk->step!=s0->step) safe=0;
                            if(sk->c0!=s0->c0||sk->c1!=s0->c1) safe=0;
                            if(safe){
                                for(j=0;j<nch;++j){
                                    int16_t a0=(int16_t)(s0->iq+(int16_t)(j*CHUNK*s0->step));
                                    int16_t a1=(int16_t)(sk->iq+(int16_t)(j*CHUNK*sk->step));
                                    if(crossed(phase_of(a0),phase_delta(a0,a1),e)){ safe=0; break; }
                                }
                            }
                            if(!safe&&pred<0) pred=k;
                            if(predp<0&&(sk->prog!=s0->prog||sk->c0!=s0->c0||sk->c1!=s0->c1))
                                predp=k;
                            if(preds<0&&sk->prog!=s0->prog) preds=k;
                            if(firstp<0&&(sk->prog!=s0->prog||sk->c0!=s0->c0||sk->c1!=s0->c1)) firstp=k;
                            if(firsts<0&&sk->prog!=s0->prog) firsts=k;
                            if(obs<0&&(sk->traj!=s0->traj||!cov_eq(sk->cover,s0->cover))){
                                obs=k;
                                if(sk->step!=s0->step) why=C_STEP;
                                else if(sk->c0!=s0->c0||sk->c1!=s0->c1) why=C_ENDPOINT;
                                else {
                                    int cr=0;
                                    for(j=0;j<nch;++j){
                                        int16_t a0=(int16_t)(s0->iq+(int16_t)(j*CHUNK*s0->step));
                                        int16_t a1=(int16_t)(sk->iq+(int16_t)(j*CHUNK*sk->step));
                                        if(crossed(phase_of(a0),phase_delta(a0,a1),e)){ cr=1; break; }
                                    }
                                    why = cr ? C_PHASE : C_NONE;
                                }
                            }
                            if(pred>=0&&obs>=0&&predp>=0&&preds>=0) break;
                        }
                        if(pred<0) pred=KSTEPS+1;
                        if(predp<0) predp=KSTEPS+1;
                        if(preds<0) preds=KSTEPS+1;
                        if(obs<0) obs=KSTEPS+1;
                        if(firstp<0) firstp=KSTEPS+1;
                        if(firsts<0) firsts=KSTEPS+1;
                        certhist[path][mode][0][pred>KSTEPS+1?KSTEPS+1:pred]++;
                        certhist[path][mode][1][predp>KSTEPS+1?KSTEPS+1:predp]++;
                        certhist[path][mode][2][preds>KSTEPS+1?KSTEPS+1:preds]++;
                        certhist[path][mode][3][obs>KSTEPS+1?KSTEPS+1:obs]++;
                        /* each tier against the quantity IT claims */
                        if(pred>obs)    ++viol_c[path][mode][0];
                        if(predp>firstp) ++viol_c[path][mode][1];
                        if(preds>firsts) ++viol_c[path][mode][2];
                        ++spans[path][mode];
                        ++cause[path][mode][why];
                        ++predhist[path][mode][pred>KSTEPS+1?KSTEPS+1:pred];
                        ++obshist[path][mode][obs>KSTEPS+1?KSTEPS+1:obs];
                        if(pred>obs){ ++viol[path][mode];        /* claimed safe too long */
                            if(g_dbg<4){ ++g_dbg;
                                Frame fk; Span *sk; int16_t qx=px,qy=py; unsigned qyaw=yaw;
                                if(mode==M_X) qx=(int16_t)(px+obs);
                                else if(mode==M_Y) qy=(int16_t)(py+obs);
                                else qyaw=(yaw+(unsigned)obs)&255u;
                                sample(qx,qy,qyaw,path,&fk); sk=find(&fk,s0->sid);
                                printf("VIOL path=%d mode=%d sid=%u pred=%d obs=%d  "
                                       "iq %d->%d step %d->%d cols %u-%u -> %u-%u  phase %d->%d\n",
                                       path,mode,s0->sid,pred,obs,s0->iq,sk?sk->iq:0,
                                       s0->step,sk?sk->step:0,s0->c0,s0->c1,
                                       sk?sk->c0:0,sk?sk->c1:0,
                                       phase_of(s0->iq),sk?phase_of(sk->iq):0);
                                if(sk) printf("     traj %s   cover %s\n",
                                    sk->traj==s0->traj?"same":"DIFFER",
                                    cov_eq(sk->cover,s0->cover)?"same":"DIFFER");
                            } }
                        else if(pred<obs) ++conserv[path][mode];
                        else ++exactpred[path][mode];
                        if(mode==0) ++marginhist[path][marg>KSTEPS+1?KSTEPS+1:marg];
                        if(g_csv&&((++g_row)&63)==0) fprintf(g_csv,"%s,%d,%u,%d,%d,%d,%d,%d,%d,%s\n",
                            path?"true":"bearing_q12",mode,s0->sid,phase_of(s0->iq),s0->step,
                            (int)(s0->c1-s0->c0+1),marg,pred,obs,CN[why]);
                    }
                }
            }
        }
    }
    if(g_csv) fclose(g_csv);

    printf("Rung 3: does the analytic phase-band certificate predict temporal stability?\n");
    printf("  poses: every %uth walkable cell x %ux%u sub-cell x %u headings, %d motion steps\n\n",
           cell_stride,sub,sub,256u/yaw_step,KSTEPS);
    for(path=0;path<2;++path){
        printf("%s bearings\n",path?"continuous-truth":"bearing_q12 (shipped)");
        printf("  %-16s %10s %12s %12s %12s\n","motion","spans","UNSAFE","conservative","exact");
        for(mode=0;mode<M_N;++mode){
            double d=(double)(spans[path][mode]?spans[path][mode]:1);
            printf("  %-16s %10lu %7lu %4.2f%% %7lu %4.1f%% %7lu %4.1f%%\n",MN[mode],
                   spans[path][mode],viol[path][mode],100.0*viol[path][mode]/d,
                   conserv[path][mode],100.0*conserv[path][mode]/d,
                   exactpred[path][mode],100.0*exactpred[path][mode]/d);
        }
        printf("  UNSAFE = the certificate claimed safe after the raster had already changed.\n");
        printf("  It must be zero; conservative is fine (wakes early, never late).\n");
        printf("\n  why the raster actually changed\n");
        for(mode=0;mode<M_N;++mode){
            int c; double d=(double)(spans[path][mode]?spans[path][mode]:1);
            printf("   %-16s",MN[mode]);
            for(c=0;c<C_N;++c) printf("  %s %.1f%%",CN[c],100.0*cause[path][mode][c]/d);
            printf("\n");
        }
        printf("\n  how long each certificate holds, in player-motion steps\n");
        printf("   %-16s %-28s %7s %5s %9s %-26s %s\n","motion","certificate","median","p90",
               "safe at 32","certifies that","UNSAFE");
        for(mode=0;mode<M_N;++mode){
            int ci;
            for(ci=0;ci<CERT_N;++ci){
                unsigned long tot=0,acc=0; int k; double p50=-1,p90=-1;
                for(k=0;k<=KSTEPS+1;++k) tot+=certhist[path][mode][ci][k];
                for(k=0;k<=KSTEPS+1;++k){ acc+=certhist[path][mode][ci][k];
                    if(p50<0&&acc*2>=tot) p50=k-1;
                    if(p90<0&&acc*10>=tot*9) p90=k-1; }
                printf("   %-16s %-28s %7.0f %5.0f %8.1f%% %-26s %s\n",
                       ci?"":MN[mode],CERTN[ci],p50,p90,
                       100.0*certhist[path][mode][ci][KSTEPS+1]/(double)(tot?tot:1),
                       CERTW[ci],
                       ci==CERT_N-1?"-":(viol_c[path][mode][ci]?"NONZERO":"0"));
            }
        }
        printf("\n");
    }
    return 0;
}
