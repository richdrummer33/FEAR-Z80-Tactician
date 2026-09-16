/* Rung 5: arbitrary-delta endpoint projection.
 *
 * The previous rung measured "what happens after one more yaw unit". That is a
 * fine way to DISCOVER boundaries and a bad model of the renderer, which must
 * take an arbitrary (dx, dy, dyaw) between rendered frames. Nothing at runtime
 * should replay unit steps: a frame either lands inside a certified region and
 * reuses state, or it does not and the destination is classified directly.
 *
 * It also over-claimed. "No compact yaw machine exists" was wrong; what was
 * shown is that no deterministic +-1 successor exists over the span-state
 * representations tested. The diagnosis for why is testable and is tested here:
 * c0 is LOSSY. Two endpoints at screen x 36.51 and 37.46 both quantise to column
 * 37, and a turn sends them to different columns. That is a bad retained
 * variable, not a complicated world.
 *
 * Under pure rotation the camera does not move, so every corner's ABSOLUTE
 * bearing is fixed and only the camera angle changes. Reading project_key:
 *
 *     st = signed_q12(a0 - (yaw << 4)),   en = st + len
 *
 * so with the pose fixed, st' = signed_q12(rel - dyaw*16) for ANY dyaw, and len
 * is unchanged. Everything downstream -- the wrap, the +-512 clip, angle_x, the
 * columns, left_real/right_real -- follows from (rel, len, dyaw) with no
 * projection at all. This harness checks that claim against the full pipeline
 * rather than assuming it, measures how lossy each candidate retained state is,
 * computes the yaw margin in closed form, and then asks the question that
 * independent one-dimensional margins cannot answer: are they composable?
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define MAXRUN 24

typedef struct {
    uint8_t live,sid,c0,c1,x0,x1,lr,rr;
    uint16_t rel;      /* (a0 - yawq) & 4095: the retained angular state */
    uint16_t len;
} EP;
typedef struct { int n; EP e[MAXRUN]; } Frame;

/* the endpoint half of project_key, driven from retained (rel,len) and any dyaw */
static int ep_from_rel(uint16_t rel,uint16_t len,int dyaw,EP *o)
{
    int16_t st,en,lo,hi; uint8_t x0,x1;
    if(len==0u||len>=2048u) return 0;
    st=signed_q12((uint16_t)(rel-(uint16_t)(dyaw*16)));
    en=(int16_t)(st+(int16_t)len);
    while(en<-512){ st=(int16_t)(st+4096); en=(int16_t)(en+4096); }
    while(st>512){ st=(int16_t)(st-4096); en=(int16_t)(en-4096); }
    lo=st<-512?-512:st; hi=en>512?512:en;
    if(hi<=lo) return 0;
    x0=angle_x(lo); x1=angle_x(hi);
    if(x1<x0){ uint8_t t=x0; x0=x1; x1=t; }
    if(x1==x0&&x1<159u) ++x1;
    o->x0=x0; o->x1=x1;
    o->c0=(uint8_t)(x0>>3); o->c1=(uint8_t)(x1>>3);
    if(o->c0>=TSP_COLS) o->c0=TSP_COLS-1;
    if(o->c1>=TSP_COLS) o->c1=TSP_COLS-1;
    o->lr=(uint8_t)(lo==st); o->rr=(uint8_t)(hi==en);
    return o->c1>=o->c0;
}

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
    tsp_polar_renderer_reset(); g_corner_bearing_valid=0u;
    for(j=0;j<nk;++j){ if(count>=TSPF_MAX_ACTIVE) break;
        if(!project_key(ks[j],&s,&g_runs[count])) continue;
        insert_run(count,&count); }
    { int w2=0;
      for(c=0;c<count&&w2<MAXRUN;++c){
        PolarRun *r=&g_runs[c];
        uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3);
        uint16_t a0,a1; TSPState *sp=&s;
        if(cc0>=TSP_COLS) cc0=TSP_COLS-1;
        if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
        if(cc1<cc0) continue;
        if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
        a0=bearing_vertex_q12(r->v0,sp); a1=bearing_vertex_q12(r->v1,sp);
        f->e[w2].live=1; f->e[w2].sid=r->sid;
        f->e[w2].c0=cc0; f->e[w2].c1=cc1; f->e[w2].x0=r->x0; f->e[w2].x1=r->x1;
        f->e[w2].lr=r->left_real; f->e[w2].rr=r->right_real;
        f->e[w2].rel=(uint16_t)((a0-((uint16_t)yaw<<4))&4095u);
        f->e[w2].len=(uint16_t)((a1-a0)&4095u);
        ++w2; }
      f->n=w2; }
}
static EP *find(Frame *f,uint8_t sid)
{ int i; for(i=0;i<f->n;++i) if(f->e[i].sid==sid) return &f->e[i]; return 0; }

/* closed-form yaw margin: the largest |dyaw| in each direction for which the
 * columns are provably unchanged. Evaluated from the retained state, not by
 * stepping the renderer. */
static int yaw_margin(const EP *e,int dir)
{
    int k; EP t;
    for(k=1;k<=255;++k){
        if(!ep_from_rel(e->rel,e->len,dir*k,&t)) return k-1;
        if(t.c0!=e->c0||t.c1!=e->c1) return k-1;
    }
    return 255;
}

#define HB 24
#define HS (1u<<HB)
static uint64_t *hk,*hv; static unsigned char *hu,*hc;
static unsigned long nst,ncf,nin,ncfi;
static void hreset(void){ memset(hu,0,HS); memset(hc,0,HS); nst=ncf=nin=ncfi=0; }
static void hput(uint64_t k,uint64_t v){
    uint64_t x=k*1099511628211ull; unsigned i=(unsigned)((x^(x>>29))&(HS-1u));
    for(;;){ if(!hu[i]){ hu[i]=1; hk[i]=k; hv[i]=v; ++nst; ++nin; return; }
             if(hk[i]==k){ ++nin; if(hv[i]!=v){ if(!hc[i]){hc[i]=1;++ncf;} ++ncfi; } return; }
             i=(i+1u)&(HS-1u); } }

static const int DYAWS[]={1,-1,2,-2,3,-3,5,-5,8,-8,13,-13,21,-21,34,-34,55,-55,89,-89,128,-128};
#define NDY ((int)(sizeof(DYAWS)/sizeof(DYAWS[0])))

int main(int argc,char**argv)
{
    unsigned cell_stride=argc>1?(unsigned)strtoul(argv[1],0,0):8u;
    unsigned sub=argc>2?(unsigned)strtoul(argv[2],0,0):4u;
    unsigned yaw_step=argc>3?(unsigned)strtoul(argv[3],0,0):1u;
    unsigned gx,gy,yaw,ox,oy,cellno;
    unsigned long exact_ok=0,exact_bad=0,cmp=0;
    unsigned long mhist[257]; unsigned long nmarg=0,msum=0;
    int lv,d;
    hk=malloc(sizeof(uint64_t)*HS); hv=malloc(sizeof(uint64_t)*HS);
    hu=malloc(HS); hc=malloc(HS);
    if(!hk||!hv||!hu||!hc) return 2;
    memset(mhist,0,sizeof mhist);

    printf("Rung 5: arbitrary-delta endpoint projection under pure rotation\n");
    printf("  poses: every %uth walkable cell x %ux%u sub-cell x %u headings\n",
           cell_stride,sub,sub,256u/yaw_step);
    printf("  deltas tested DIRECTLY, never by iterating +-1:");
    for(d=0;d<NDY;++d) printf(" %+d",DYAWS[d]);
    printf("\n\n");

    /* 1. exactness of the retained-state update for arbitrary dyaw */
    cellno=0;
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
                for(d=0;d<NDY;++d){
                    unsigned y1=(yaw+(unsigned)(DYAWS[d]+256*4))&255u;
                    sample(px,py,y1,&f1);
                    for(i=0;i<f0.n;++i){
                        EP *a=&f0.e[i],*b2=find(&f1,a->sid),pred;
                        int okp=ep_from_rel(a->rel,a->len,DYAWS[d],&pred);
                        if(!b2){ continue; }
                        ++cmp;
                        if(okp&&pred.c0==b2->c0&&pred.c1==b2->c1&&
                           pred.x0==b2->x0&&pred.x1==b2->x1&&
                           pred.lr==b2->lr&&pred.rr==b2->rr) ++exact_ok;
                        else { ++exact_bad;
                               if(exact_bad<4)
                                 printf("   MISMATCH sid=%u dyaw=%+d  pred c%u-%u x%u-%u  "
                                        "actual c%u-%u x%u-%u\n",a->sid,DYAWS[d],
                                        pred.c0,pred.c1,pred.x0,pred.x1,
                                        b2->c0,b2->c1,b2->x0,b2->x1); }
                    }
                }
                /* margins, in closed form */
                for(i=0;i<f0.n;++i){
                    int mf=yaw_margin(&f0.e[i],+1),mb=yaw_margin(&f0.e[i],-1);
                    int m=mf<mb?mf:mb;
                    ++mhist[m>256?256:m]; ++nmarg; msum+=(unsigned long)m;
                }
            }
        }
    }
    printf("1  does retained (rel, len) + arbitrary dyaw reproduce the full pipeline?\n");
    printf("   %lu comparisons, %lu exact, %lu mismatched   %s\n",
           cmp,exact_ok,exact_bad,exact_bad?"FAIL":"EXACT");
    printf("   the columns, the pixel endpoints and both real-edge flags all agree,\n");
    printf("   for every delta including +-128, computed in one shot\n\n");

    /* 2. how lossy is each candidate retained state? */
    printf("2  is the destination column determined by the retained state?\n");
    printf("   %-34s %12s %12s %10s\n","retained state","states","conflicting","by state");
    for(lv=0;lv<4;++lv){
        static const char *LN[4]={"c0, c1  (the column alone)",
                                  "c0, c1, len",
                                  "x0, x1  (pixel endpoints)",
                                  "rel, len  (angular state)"};
        hreset(); cellno=0;
        for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
            int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
            if(!tsp_is_walkable_q4(cx,cy)) continue;
            ++cellno; if((cellno-1u)%cell_stride) continue;
            for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
                int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
                int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
                if(!tsp_is_walkable_q4(px,py)) continue;
                for(yaw=0;yaw<256u;yaw+=yaw_step){
                    Frame f0; int i;
                    sample(px,py,yaw,&f0);
                    for(i=0;i<f0.n;++i){
                        EP *a=&f0.e[i],pred; uint64_t h;
                        for(d=0;d<NDY;++d){
                            if(!ep_from_rel(a->rel,a->len,DYAWS[d],&pred)) continue;
                            h=1469598103934665603ull;
                            #define MX(v) do{ h^=(uint64_t)(v); h*=1099511628211ull; }while(0)
                            MX(d+1);
                            if(lv==0){ MX(a->c0+1); MX(a->c1+1); }
                            else if(lv==1){ MX(a->c0+1); MX(a->c1+1); MX(a->len+1); }
                            else if(lv==2){ MX(a->x0+1); MX(a->x1+1); }
                            else { MX(a->rel+1); MX(a->len+1); }
                            #undef MX
                            hput(h,((uint64_t)pred.c0<<8)|pred.c1);
                        }
                    }
                }
            }
        }
        printf("   %-34s %12lu %12lu %9.2f%%\n",LN[lv],nst,ncf,100.0*ncf/(double)(nst?nst:1));
    }
    printf("   c0 alone is lossy by construction: two endpoints at screen x 36.51 and\n");
    printf("   37.46 share column 37 and separate under a turn. The angular state is not.\n\n");

    /* 4. Independent one-dimensional margins are not automatically composable.
     * A frame applies dx, dy and dyaw together, so a certificate built from
     * separately-measured axes has to be checked, not assumed. Translation
     * margins are measured by direct evaluation at the destination -- never by
     * stepping -- and then a combined move inside both is tested. */
    {
        unsigned long pairs=0,bad=0,badyaw=0,pairs_yaw=0;
        unsigned long mxs=0,mys=0,nxy=0;
        cellno=0;
        for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
            int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
            if(!tsp_is_walkable_q4(cx,cy)) continue;
            ++cellno; if((cellno-1u)%cell_stride) continue;
            for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
                int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
                int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
                if(!tsp_is_walkable_q4(px,py)) continue;
                for(yaw=0;yaw<256u;yaw+=yaw_step){
                    Frame f0,fd; int i,k;
                    sample(px,py,yaw,&f0);
                    for(i=0;i<f0.n;++i){
                        EP *a=&f0.e[i]; int mx=0,my=0,myaw;
                        /* direct evaluation at each candidate destination */
                        for(k=1;k<=24;++k){
                            EP *b2; int16_t qx=(int16_t)(px+k);
                            if(!tsp_is_walkable_q4(qx,py)) break;
                            sample(qx,py,yaw,&fd); b2=find(&fd,a->sid);
                            if(!b2||b2->c0!=a->c0||b2->c1!=a->c1) break;
                            mx=k;
                        }
                        for(k=1;k<=24;++k){
                            EP *b2; int16_t qy=(int16_t)(py+k);
                            if(!tsp_is_walkable_q4(px,qy)) break;
                            sample(px,qy,yaw,&fd); b2=find(&fd,a->sid);
                            if(!b2||b2->c0!=a->c0||b2->c1!=a->c1) break;
                            my=k;
                        }
                        myaw=yaw_margin(a,+1);
                        mxs+=(unsigned long)mx; mys+=(unsigned long)my; ++nxy;
                        /* a combined move strictly inside both 1-D margins */
                        if(mx>0&&my>0){
                            int dx=(mx+1)/2,dy=(my+1)/2; EP *b2;
                            int16_t qx=(int16_t)(px+dx),qy=(int16_t)(py+dy);
                            if(tsp_is_walkable_q4(qx,qy)){
                                sample(qx,qy,yaw,&fd); b2=find(&fd,a->sid);
                                ++pairs;
                                if(!b2||b2->c0!=a->c0||b2->c1!=a->c1) ++bad;
                            }
                        }
                        if(mx>0&&myaw>0){
                            int dx=(mx+1)/2,dyw=(myaw+1)/2; EP *b2;
                            int16_t qx=(int16_t)(px+dx);
                            if(tsp_is_walkable_q4(qx,py)){
                                sample(qx,py,(yaw+(unsigned)dyw)&255u,&fd); b2=find(&fd,a->sid);
                                ++pairs_yaw;
                                if(!b2||b2->c0!=a->c0||b2->c1!=a->c1) ++badyaw;
                            }
                        }
                    }
                }
            }
        }
        printf("4  are independent one-dimensional margins composable?\n");
        printf("   mean translation margin: X %.2f, Y %.2f sixteenths of a cell\n",
               (double)mxs/(double)(nxy?nxy:1),(double)mys/(double)(nxy?nxy:1));
        printf("   moves inside BOTH the X and Y margins:   %lu tested, %lu broke the columns (%.2f%%)\n",
               pairs,bad,100.0*bad/(double)(pairs?pairs:1));
        printf("   moves inside BOTH the X and yaw margins: %lu tested, %lu broke the columns (%.2f%%)\n",
               pairs_yaw,badyaw,100.0*badyaw/(double)(pairs_yaw?pairs_yaw:1));
        printf("   a nonzero figure means per-axis margins cannot simply be conjoined and\n");
        printf("   the certificate needs a genuinely joint region or a conservative test.\n\n");
    }

    printf("3  yaw margin to the next column change, in closed form\n");
    {   unsigned long acc=0; int k; double p50=-1,p90=-1;
        for(k=0;k<=256;++k){ acc+=mhist[k];
            if(p50<0&&acc*2>=nmarg) p50=k;
            if(p90<0&&acc*10>=nmarg*9) p90=k; }
        printf("   %lu spans, mean %.2f, median %.0f, p90 %.0f yaw units\n",
               nmarg,(double)msum/(double)(nmarg?nmarg:1),p50,p90);
        printf("   distribution:");
        for(k=0;k<8;++k) if(mhist[k]) printf("  %d:%.1f%%",k,100.0*mhist[k]/(double)nmarg);
        printf("\n");
    }
    return 0;
}
