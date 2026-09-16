/* Rung 6: arbitrary-pose destination evaluation, on all three axes at once.
 *
 * The architecture this is testing is deliberately stateless at the bottom:
 *
 *     stateless arbitrary-delta DESTINATION EVALUATION is the foundation;
 *     temporal certificates are optional shortcuts on top of it.
 *
 * Correctness must never depend on how fast the player moved or on replaying
 * what was crossed. So every delta here is applied in ONE SHOT and the result is
 * compared against the full renderer at the destination pose. Deltas that cross
 * quadtree leaves, coarse cells, and several of both, are first-class cases --
 * not an afterthought -- because the pleasant same-leaf case would otherwise be
 * the only thing proved.
 *
 * What is retained per run is only its identity and its angular state:
 *      (sid, v0, v1)   which wall and which two corners
 *      (rel, len)      the angular state, for the rotation shortcut
 *
 * Destination evaluation then does, with no traversal of anything crossed:
 *      locate the destination coarse cell and quadtree leaf
 *      evaluate the baked corner bearings there
 *      subtract the destination yaw          (exact, closed form, any dyaw)
 *      angle_x  ->  x0, x1  ->  c0, c1
 *      wall distance -> invd -> dp_derive -> iq, step
 *      closed form -> the raster
 *
 * Two translation updates are compared, because they are not the same thing:
 *   DEST   evaluate the destination leaf's affine record at the destination
 *          position. Always available, costs one leaf lookup and two products.
 *   INCR   add Ax*dx + Ay*dy to the retained bearing without re-reading the
 *          leaf. Cheaper, but only meaningful inside one leaf, and the ROM's
 *          per-term truncate-toward-zero means it is NOT obviously equal to
 *          DEST even there. Measured rather than assumed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static uint64_t raster_hash(int16_t iq,int16_t step,int n,int c0abs)
{
    uint64_t h=1469598103934665603ull; int fam,left=n,cs=0;
    for(fam=0;fam<=2;fam+=2){
        left=n; cs=0;
        while(left>0){
            int want=left>CHUNK?CHUNK:left,is_last=(left-want==0),c,row,col=0;
            int lo[CHUNK+2],hi[CHUNK+2],ncols=is_last?want+1:want+2;
            int16_t q=(int16_t)(iq+(int16_t)(cs*step));
            for(c=0;c<ncols;++c) if(!in_domain(q,step,c)) return 0ull;
            for(c=0;c<(is_last?want:want+1);++c){
                int a=yq(q,step,c,fam),b=yq(q,step,c+1,fam);
                lo[c]=(a<=b?a:b)>>3; hi[c]=(a<=b?b:a)>>3; }
            row=lo[0];
            for(c=0;c<want;++c){
                int k,jump;
                for(k=0;k<hi[c]-lo[c];++k){ h^=(uint64_t)(row*20+c0abs+cs+col+1); h*=1099511628211ull; ++row; }
                h^=(uint64_t)(row*20+c0abs+cs+col+1); h*=1099511628211ull;
                if(is_last&&c==want-1) break;
                jump=lo[c+1]-hi[c];
                if(jump>0||jump<-4) return 0ull;
                h^=(uint64_t)(200+jump); h*=1099511628211ull;
                ++col; row+=jump; }
            cs+=CHUNK; left-=want; }
    }
    return h;
}

/* the endpoint half of project_key, from an angular state */
typedef struct { uint8_t ok,c0,c1,x0,x1,lr,rr; } EP;
static void ep_from_rel(uint16_t rel,uint16_t len,int dyaw,EP *o)
{
    int16_t st,en,lo,hi; uint8_t x0,x1;
    memset(o,0,sizeof *o);
    if(len==0u||len>=2048u) return;
    st=signed_q12((uint16_t)(rel-(uint16_t)(dyaw*16)));
    en=(int16_t)(st+(int16_t)len);
    while(en<-512){ st=(int16_t)(st+4096); en=(int16_t)(en+4096); }
    while(st>512){ st=(int16_t)(st-4096); en=(int16_t)(en-4096); }
    lo=st<-512?-512:st; hi=en>512?512:en;
    if(hi<=lo) return;
    x0=angle_x(lo); x1=angle_x(hi);
    if(x1<x0){ uint8_t t=x0; x0=x1; x1=t; }
    if(x1==x0&&x1<159u) ++x1;
    o->x0=x0; o->x1=x1;
    o->c0=(uint8_t)(x0>>3); o->c1=(uint8_t)(x1>>3);
    if(o->c0>=TSP_COLS) o->c0=TSP_COLS-1;
    if(o->c1>=TSP_COLS) o->c1=TSP_COLS-1;
    if(o->c1<o->c0) return;
    o->lr=(uint8_t)(lo==st); o->rr=(uint8_t)(hi==en); o->ok=1;
}

/* baked bearing for a corner at an arbitrary position; loads that position's
 * cell and leaf. Returns 0 where the bake falls back, in which case the ROM
 * itself uses bearing_q12 and so does this. */
static uint16_t bearing_at(unsigned v,int16_t px,int16_t py)
{
    unsigned gx=(unsigned)(px/CELL_Q4),gy=(unsigned)(py/CELL_Q4);
    uint16_t b;
    if(proj_host_load(gx,gy)&&proj_host_bearing(v,px,py,&b)) return b;
    return bearing_q12((int16_t)(((int16_t)k_tspf_vx[v]<<4)-px),
                       (int16_t)(((int16_t)k_tspf_vy[v]<<4)-py));
}
/* leaf identity of a corner at a position, for classifying the delta */
static long leaf_id(unsigned v,int16_t px,int16_t py)
{
    unsigned gx=(unsigned)(px/CELL_Q4),gy=(unsigned)(py/CELL_Q4),sh,n;
    if(!proj_host_load(gx,gy)) return -1;
    if(g_pj_depth[v]==0xffu) return -2;
    sh=6u-g_pj_depth[v]; n=1u<<g_pj_depth[v];
    return (long)((gy*GRID_W+gx)*4096u
                  + ((((unsigned)py&63u)>>sh)*n + (((unsigned)px&63u)>>sh)));
}

typedef struct { uint8_t live,sid,v0,v1; uint16_t rel,len; EP ep; int16_t iq,step; uint64_t ras; } Run;
typedef struct { int n; Run r[MAXRUN]; } Frame;

static void sample(int16_t px,int16_t py,unsigned yaw,Frame *f)
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
    /* the ROM's own baked-bearing path */
    tsp_polar_renderer_reset(); g_corner_bearing_valid=0u;
    { unsigned v; uint16_t bb;
      if(proj_host_load(gx,gy)) for(v=0;v<14u;++v)
        if(proj_host_bearing(v,px,py,&bb)){ g_corner_bearing_q12[v]=bb;
                                            g_corner_bearing_valid|=k_corner_mask[v]; } }
    for(j=0;j<nk;++j){ if(count>=TSPF_MAX_ACTIVE) break;
        if(!project_key(ks[j],&s,&g_runs[count])) continue; insert_run(count,&count); }
    { int w=0;
      for(c=0;c<count&&w<MAXRUN;++c){
        PolarRun *r=&g_runs[c]; Run *o;
        uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),invd;
        if(cc0>=TSP_COLS) cc0=TSP_COLS-1;
        if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
        if(cc1<cc0) continue;
        if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
        o=&f->r[w++]; memset(o,0,sizeof *o);
        o->live=1; o->sid=r->sid; o->v0=r->v0; o->v1=r->v1;
        o->rel=(uint16_t)((bearing_vertex_q12(r->v0,&s)-((uint16_t)yaw<<4))&4095u);
        o->len=(uint16_t)((bearing_vertex_q12(r->v1,&s)-bearing_vertex_q12(r->v0,&s))&4095u);
        o->ep.ok=1; o->ep.c0=cc0; o->ep.c1=cc1; o->ep.x0=r->x0; o->ep.x1=r->x1;
        o->ep.lr=r->left_real; o->ep.rr=r->right_real;
        invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],&s));
        if(dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&o->iq,&o->step))
            o->ras=raster_hash(o->iq,o->step,(int)(cc1-cc0+1),cc0);
      }
      f->n=w; }
}
/* Match on the CORNER PAIR, not just the wall. One sid can be reached through
 * several keys with different (v0,v1), and matching on sid alone paired a
 * retained run against a different key's run at the destination -- which showed
 * up as a1 agreeing exactly while a0 was 477 units out. That was the harness,
 * not the architecture. */
static Run *find(Frame *f,uint8_t sid,uint8_t v0,uint8_t v1)
{ int i; for(i=0;i<f->n;++i)
    if(f->r[i].sid==sid&&f->r[i].v0==v0&&f->r[i].v1==v1) return &f->r[i];
  return 0; }

enum { K_SAME=0,K_LEAF=1,K_CELL=2,K_FAR=3,K_N=4 };
static const char *KN[K_N]={"same leaf","crosses a leaf","crosses a coarse cell",
                            "crosses several cells"};
enum { D_T=0,D_R=1,D_C=2,D_N=3 };
static const char *DN[D_N]={"translation only","rotation only","combined dx,dy,dyaw"};
static unsigned long tot[D_N][K_N],ep_bad[D_N][K_N],dda_bad[D_N][K_N],ras_bad[D_N][K_N];
static unsigned long incr_n,incr_bad;
static int g_dbg=0;

int main(int argc,char**argv)
{
    unsigned cell_stride=argc>1?(unsigned)strtoul(argv[1],0,0):8u;
    unsigned sub=argc>2?(unsigned)strtoul(argv[2],0,0):4u;
    unsigned yaw_step=argc>3?(unsigned)strtoul(argv[3],0,0):16u;
    static const int DL[]={1,3,7,13,21,34,55,89,144};
    static const int ND=(int)(sizeof(DL)/sizeof(DL[0]));
    unsigned gx,gy,yaw,ox,oy,cellno=0; int di,dk,dm;
    unsigned long seed=99u;

    printf("Rung 6: arbitrary-pose destination evaluation\n");
    printf("  every delta applied in ONE SHOT, never by stepping; boundary crossings\n");
    printf("  and combined (dx,dy,dyaw) are first-class cases\n");
    printf("  deltas:"); for(di=0;di<ND;++di) printf(" %+d",DL[di]); printf(" (and negatives)\n\n");

    for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
        int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
        if(!tsp_is_walkable_q4(cx,cy)) continue;
        ++cellno; if((cellno-1u)%cell_stride) continue;
        for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
            int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
            int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
            if(!tsp_is_walkable_q4(px,py)) continue;
            for(yaw=0;yaw<256u;yaw+=yaw_step){
                Frame f0,f1; int i;
                sample(px,py,yaw,&f0);
                if(!f0.n) continue;
                for(dm=0;dm<D_N;++dm) for(di=0;di<ND;++di) for(dk=0;dk<2;++dk){
                    int m=DL[di]*(dk?-1:1),dx=0,dy=0,dyw=0;
                    int16_t qx,qy; unsigned qyaw;
                    seed=seed*1103515245u+12345u;
                    if(dm==D_T){ dx=m; dy=(int)((seed>>16)%7u)-3; }
                    else if(dm==D_R){ dyw=m; }
                    else { dx=m; dy=-m/2; dyw=(int)((seed>>16)%17u)-8; }
                    qx=(int16_t)(px+dx); qy=(int16_t)(py+dy);
                    qyaw=(unsigned)(((int)yaw+dyw)&255);
                    if(qx<0||qy<0) continue;
                    if(!tsp_is_walkable_q4(qx,qy)) continue;
                    sample(qx,qy,qyaw,&f1);
                    for(i=0;i<f0.n;++i){
                        Run *a=&f0.r[i],*b2=find(&f1,a->sid,a->v0,a->v1);
                        EP pred; uint16_t a0,a1,rel2,len2; int kcls;
                        int16_t piq,pstep; uint8_t pinvd; TSPState ss;
                        long l0,l1;
                        if(!b2) continue;               /* visibility event, not an error */
                        /* classify the delta by what it crossed */
                        l0=leaf_id(a->v0,px,py); l1=leaf_id(a->v0,qx,qy);
                        if((unsigned)(px/CELL_Q4)!=(unsigned)(qx/CELL_Q4)||
                           (unsigned)(py/CELL_Q4)!=(unsigned)(qy/CELL_Q4))
                            kcls = (abs(dx)+abs(dy)>=CELL_Q4) ? K_FAR : K_CELL;
                        else kcls = (l0==l1) ? K_SAME : K_LEAF;
                        /* ---- one-shot destination evaluation ---- */
                        a0=bearing_at(a->v0,qx,qy); a1=bearing_at(a->v1,qx,qy);
                        len2=(uint16_t)((a1-a0)&4095u);
                        rel2=(uint16_t)((a0-((uint16_t)qyaw<<4))&4095u);
                        ep_from_rel(rel2,len2,0,&pred);
                        ++tot[dm][kcls];
                        if(!pred.ok||pred.c0!=b2->ep.c0||pred.c1!=b2->ep.c1||
                           pred.x0!=b2->ep.x0||pred.x1!=b2->ep.x1||
                           pred.lr!=b2->ep.lr||pred.rr!=b2->ep.rr){
                            ++ep_bad[dm][kcls];
                            if(g_dbg<8){ ++g_dbg;
                              printf("   EPMISS sid=%u v0=%u v1=%u d=(%d,%d,%d) %s\n"
                                     "      pred c%u-%u x%u-%u lr%u rr%u   actual c%u-%u x%u-%u lr%u rr%u\n"
                                     "      pred rel=%u len=%u   actual rel=%u len=%u  a0=%u a1=%u\n",
                                     a->sid,a->v0,a->v1,dx,dy,dyw,KN[kcls],
                                     pred.c0,pred.c1,pred.x0,pred.x1,pred.lr,pred.rr,
                                     b2->ep.c0,b2->ep.c1,b2->ep.x0,b2->ep.x1,b2->ep.lr,b2->ep.rr,
                                     rel2,len2,b2->rel,b2->len,a0,a1); }
                            continue; }
                        memset(&ss,0,sizeof ss); ss.x_q4=qx; ss.y_q4=qy; ss.yaw=(uint8_t)qyaw;
                        pinvd=inv_for_dq4(wall_d_q4(a->sid,k_tspf_seg_anchor[a->sid],&ss));
                        if(!dp_derive(a->sid,pinvd,pred.c0,pred.c1,(uint8_t)qyaw,&piq,&pstep)) continue;
                        if(piq!=b2->iq||pstep!=b2->step){ ++dda_bad[dm][kcls]; continue; }
                        if(raster_hash(piq,pstep,(int)(pred.c1-pred.c0+1),pred.c0)!=b2->ras)
                            ++ras_bad[dm][kcls];
                        /* ---- the incremental affine update, same leaf only ---- */
                        if(kcls==K_SAME&&dm!=D_R&&g_pj_depth[a->v0]!=0xffu){
                            unsigned sh; const uint8_t *rec; unsigned lxq,lyq,n2,leafx,leafy;
                            proj_host_load((unsigned)(qx/CELL_Q4),(unsigned)(qy/CELL_Q4));
                            if(g_pj_depth[a->v0]!=0xffu){
                                sh=6u-g_pj_depth[a->v0]; n2=1u<<g_pj_depth[a->v0];
                                lxq=(unsigned)((uint16_t)qx&63u); lyq=(unsigned)((uint16_t)qy&63u);
                                leafx=lxq>>sh; leafy=lyq>>sh;
                                rec=g_pj_leaf[a->v0]+4u*(leafy*n2+leafx);
                                { int sx=(int)(int8_t)rec[2],sy=(int)(int8_t)rec[3];
                                  int oldb=(int)bearing_at(a->v0,px,py);
                                  int inc=oldb + ((sx*dx)>>sh) + ((sy*dy)>>sh);
                                  ++incr_n;
                                  if(((unsigned)inc&4095u)!=a0) ++incr_bad; }
                            }
                        }
                    }
                }
            }
        }
    }
    printf("1  does one-shot destination evaluation reproduce the renderer?\n");
    printf("   %-22s %-24s %10s %9s %9s %9s\n","delta","what it crossed","cases",
           "endpoint","iq/step","raster");
    for(dm=0;dm<D_N;++dm) for(dk=0;dk<K_N;++dk){
        if(!tot[dm][dk]) continue;
        printf("   %-22s %-24s %10lu %9lu %9lu %9lu\n",dk?"":DN[dm],KN[dk],tot[dm][dk],
               ep_bad[dm][dk],dda_bad[dm][dk],ras_bad[dm][dk]);
    }
    { unsigned long e=0,d=0,r=0,t2=0;
      for(dm=0;dm<D_N;++dm) for(dk=0;dk<K_N;++dk){
        t2+=tot[dm][dk]; e+=ep_bad[dm][dk]; d+=dda_bad[dm][dk]; r+=ras_bad[dm][dk]; }
      printf("   %-22s %-24s %10lu %9lu %9lu %9lu   %s\n","TOTAL","",t2,e,d,r,
             (e||d||r)?"MISMATCH":"EXACT"); }
    /* Safe-region slices. Per-axis margins describe a rectangle; the real region
     * is whatever shape the coupled projection gives, and the composability
     * failures should be visible as boundaries cutting across that rectangle. */
    if(argc>4){
        char pth[512]; FILE *fp; int found=0;
        snprintf(pth,sizeof pth,"%s/safe_slices.csv",argv[4]); fp=fopen(pth,"w");
        if(fp){
            fprintf(fp,"slice,run,a,b,same\n");
            cellno=0;
            for(gy=0;gy<GRID_H&&found<4;++gy) for(gx=0;gx<GRID_W&&found<4;++gx){
                int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
                if(!tsp_is_walkable_q4(cx,cy)) continue;
                ++cellno; if((cellno-1u)%37u) continue;
                for(yaw=0;yaw<256u&&found<4;yaw+=53u){
                    Frame f0,fd; int i,A,B;
                    sample(cx,cy,yaw,&f0);
                    for(i=0;i<f0.n&&found<4;++i){
                        Run *a=&f0.r[i],*b2;
                        for(A=-24;A<=24;++A) for(B=-24;B<=24;++B){
                            int16_t qx=(int16_t)(cx+A),qy=(int16_t)(cy+B); int same=0;
                            if(qx>=0&&qy>=0&&tsp_is_walkable_q4(qx,qy)){
                                sample(qx,qy,yaw,&fd); b2=find(&fd,a->sid,a->v0,a->v1);
                                same=(b2&&b2->ep.c0==a->ep.c0&&b2->ep.c1==a->ep.c1);
                            }
                            fprintf(fp,"xy,%d,%d,%d,%d\n",found,A,B,same);
                        }
                        for(A=-24;A<=24;++A) for(B=-24;B<=24;++B){
                            int16_t qx=(int16_t)(cx+A); unsigned qyaw=(unsigned)(((int)yaw+B)&255);
                            int same=0;
                            if(qx>=0&&tsp_is_walkable_q4(qx,cy)){
                                sample(qx,cy,qyaw,&fd); b2=find(&fd,a->sid,a->v0,a->v1);
                                same=(b2&&b2->ep.c0==a->ep.c0&&b2->ep.c1==a->ep.c1);
                            }
                            fprintf(fp,"xyaw,%d,%d,%d,%d\n",found,A,B,same);
                        }
                        ++found;
                    }
                }
            }
            fclose(fp);
            printf("\n   safe-region slices written to %s (%d runs)\n",pth,found);
        }
    }

    printf("\n2  is the cheaper in-leaf incremental update equal to destination evaluation?\n");
    printf("   %lu same-leaf translations, %lu disagreed (%.3f%%)\n",
           incr_n,incr_bad,100.0*incr_bad/(double)(incr_n?incr_n:1));
    printf("   the ROM truncates each product toward zero separately, so adding\n");
    printf("   Ax*dx + Ay*dy to a retained bearing is not the same rounding as\n");
    printf("   evaluating the leaf record at the destination.\n");
    return 0;
}
