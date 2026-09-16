/* Rung 4: does the finite static machine induce a compact finite YAW machine?
 *
 * Rotation is not a separate raster problem. It moves the same inputs the static
 * machine already has: (phase, step, stop) -> (phase', step', stop'). So the
 * question is whether there is a compact state Q and a transition
 *
 *     T_yaw(Q, +-1) -> Q'
 *
 * that is DETERMINISTIC. Nothing is designed here and nothing is implemented.
 * This is partition refinement run backwards: start from the smallest plausible
 * state, measure whether every sample carrying that state produces the same
 * successor, and add a field only when the data forces it. The answer is
 * measured, not chosen.
 *
 * If the state stays small, a rotational program exists. If it balloons back
 * into raw geometry, it does not, and we say so.
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
static uint64_t chunk_sig6(int16_t iq,int16_t step)
{
    uint64_t h=1469598103934665603ull; int fam,c;
    for(fam=0;fam<2;++fam) for(c=0;c<6;++c){
        long a0=(long)iq+32+(long)c*step,a1=a0+step,a2=a1+step;
        long h0=a0>>7,h1=a1>>7,h2=a2>>7;
        long y0=fam?72+h0:71-h0,y1=fam?72+h1:71-h1,y2=fam?72+h2:71-h2;
        long lo0=(y0<y1?y0:y1)>>3,hi0=(y0<y1?y1:y0)>>3;
        long lo1=(y1<y2?y1:y2)>>3;
        h^=(uint64_t)((hi0-lo0)*8+(hi0-lo1)+64); h*=1099511628211ull;
    }
    return h;
}
static int phase_of(int16_t iq){ long v=((long)iq+32L)%1024L; if(v<0)v+=1024; return (int)v; }
static void band_edges(int16_t step,int *e)
{ int c; for(c=0;c<8;++c){ long v=-(long)c*(long)step; v%=1024L; if(v<0)v+=1024L; e[c]=(int)v; } }

/* the observable fields a rotational state could be built from */
typedef struct {
    uint8_t live,sid,c0,c1,cls,invd;
    int16_t iq,step;
    uint64_t prog,band,rows;
} Obs;
typedef struct { int n; Obs o[MAXRUN]; } Frame;

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
        if(!project_key(ks[j],&s,&g_runs[count])) continue; insert_run(count,&count); }
    for(c=0;c<count&&f->n<MAXRUN;++c){
        PolarRun *r=&g_runs[c];
        uint8_t cc0=(uint8_t)(r->x0>>3),cc1=(uint8_t)(r->x1>>3),n,invd;
        int16_t iq,step; Obs *o; int nch,jj,e[8];
        if(cc0>=TSP_COLS) cc0=TSP_COLS-1;
        if(cc1>=TSP_COLS) cc1=TSP_COLS-1;
        if(cc1<cc0) continue;
        if(k_tspf_profile[r->sid]!=TSP_PROFILE_FULL) continue;
        n=(uint8_t)(cc1-cc0+1u);
        invd=inv_for_dq4(wall_d_q4(r->sid,k_tspf_seg_anchor[r->sid],&s));
        if(!dp_derive(r->sid,invd,cc0,cc1,(uint8_t)yaw,&iq,&step)) continue;
        o=&f->o[f->n++]; memset(o,0,sizeof *o);
        o->live=1; o->sid=r->sid; o->c0=cc0; o->c1=cc1; o->iq=iq; o->step=step;
        o->invd=invd;
        o->cls=k_dp_normal_class[r->sid];
        o->prog=1469598103934665603ull; o->band=1469598103934665603ull; o->rows=1469598103934665603ull;
        nch=((int)n+CHUNK-1)/CHUNK; band_edges(step,e);
        for(jj=0;jj<nch;++jj){
            int16_t q=(int16_t)(iq+(int16_t)(jj*CHUNK*step));
            int ph=phase_of(q),bi=0,c2;
            o->prog^=chunk_sig6(q,step); o->prog*=1099511628211ull;
            for(c2=0;c2<8;++c2) if(((ph-e[c2])&1023)<512) ++bi;
            o->band^=(uint64_t)(bi+1); o->band*=1099511628211ull;
            o->rows^=(uint64_t)(((71-(((long)q+32)>>7))>>3)+256); o->rows*=1099511628211ull;
        }
    }
}
static Obs *find(Frame *f,uint8_t sid)
{ int i; for(i=0;i<f->n;++i) if(f->o[i].sid==sid) return &f->o[i]; return 0; }

/* refinement levels: each adds one field to the state */
#define LV 10
static const char *LVN[LV]={
    "prog",
    "prog + band",
    "prog + band + columns",
    "prog + band + columns + rows",
    "  + step",
    "  + step + yaw",
    "  + step + yaw + class",
    "  + step + yaw + class + iq",
    "  + invd                       ",
    "  + invd + wall id  (everything)"};
static uint64_t qkey(const Obs *o,unsigned yaw,int lv)
{
    uint64_t h=1469598103934665603ull;
    #define MIX(v) do{ h^=(uint64_t)(v); h*=1099511628211ull; }while(0)
    MIX(o->prog);
    if(lv>=1) MIX(o->band);
    if(lv>=2){ MIX(o->c0+1); MIX(o->c1+1); }
    if(lv>=3) MIX(o->rows);
    if(lv>=4) MIX((uint16_t)o->step);
    if(lv>=5) MIX(yaw+1);
    if(lv>=6) MIX(o->cls+1);
    if(lv>=7) MIX((uint16_t)o->iq);
    if(lv>=8) MIX(o->invd+1);
    if(lv>=9) MIX(o->sid+1);
    #undef MIX
    return h;
}

/* state -> successor, with a conflict flag */
#define HB 25
#define HS (1u<<HB)
static uint64_t *hk,*hv; static unsigned char *hu,*hc; static unsigned long *hn_;
static unsigned long nstate,nconf,ninst,nconf_inst;
static void hs_reset(void){ memset(hu,0,HS); memset(hc,0,HS); nstate=nconf=ninst=nconf_inst=0; }
static void hs_put(uint64_t key,uint64_t succ)
{
    uint64_t x=key*1099511628211ull; unsigned i=(unsigned)((x^(x>>29))&(HS-1u));
    for(;;){
        if(!hu[i]){ hu[i]=1; hk[i]=key; hv[i]=succ; hn_[i]=1; ++nstate; ++ninst; return; }
        if(hk[i]==key){ ++ninst; ++hn_[i];
            if(hv[i]!=succ){ if(!hc[i]){ hc[i]=1; ++nconf; } ++nconf_inst; }
            return; }
        i=(i+1u)&(HS-1u);
    }
}

int main(int argc,char**argv)
{
    unsigned cell_stride=argc>1?(unsigned)strtoul(argv[1],0,0):4u;
    unsigned sub=argc>2?(unsigned)strtoul(argv[2],0,0):6u;
    unsigned gx,gy,yaw,ox,oy,cellno=0; int lv,dir,target;
    hk=malloc(sizeof(uint64_t)*HS); hv=malloc(sizeof(uint64_t)*HS);
    hu=malloc(HS); hc=malloc(HS); hn_=malloc(sizeof(unsigned long)*HS);
    if(!hk||!hv||!hu||!hc||!hn_) return 2;

    printf("Rung 4: is there a compact deterministic yaw transition?\n");
    printf("  poses: every %uth walkable cell x %ux%u sub-cell x all 256 headings\n\n",
           cell_stride,sub,sub);
    for(target=0;target<2;++target){
    printf("%s\n",target?
      "B  successor = the next PROGRAM only. This is the question a rotational\n"
      "   program has to answer: given a compact state, what does the span emit\n"
      "   after one yaw tick?":
      "A  successor = the next FULL state. Needed if the machine is to run on its\n"
      "   own state without re-deriving anything.");
    printf("  %-38s %12s %12s %10s %10s\n","state (partition refinement)","states",
           "conflicting","by state","by instance");
    for(lv=0;lv<LV;++lv){
        unsigned long S=0,C=0,I=0,CI=0;
        for(dir=0;dir<2;++dir){
            hs_reset();
            cellno=0;
            for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
                int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
                if(!tsp_is_walkable_q4(cx,cy)) continue;
                ++cellno; if((cellno-1u)%cell_stride) continue;
                for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
                    int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
                    int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
                    if(!tsp_is_walkable_q4(px,py)) continue;
                    for(yaw=0;yaw<256u;++yaw){
                        Frame f0,f1; int i;
                        unsigned y1=dir? (yaw+255u)&255u : (yaw+1u)&255u;
                        sample(px,py,yaw,&f0);
                        if(!f0.n) continue;
                        sample(px,py,y1,&f1);
                        for(i=0;i<f0.n;++i){
                            Obs *a=&f0.o[i],*b2=find(&f1,a->sid);
                            uint64_t succ = b2 ? (target? b2->prog : qkey(b2,y1,lv)) : 0xDEADull;
                            hs_put(qkey(a,yaw,lv),succ);
                        }
                    }
                }
            }
            S+=nstate; C+=nconf; I+=ninst; CI+=nconf_inst;
        }
        /* S and C are both totals over the two directions, so the ratio is a
         * real share. Dividing states by two while summing conflicts produced a
         * "137%" in the first run, which is how the bug announced itself. */
        printf("  %-38s %12lu %12lu %9.2f%% %9.2f%%\n",LVN[lv],S,C,
               100.0*C/(double)(S?S:1),100.0*CI/(double)(I?I:1));
    }
    printf("\n");
    }
    /* Adding iq, invd and the wall id barely moved the conflict rate, which says
     * the successor depends on something the span does not carry: the projected
     * columns at the NEXT yaw, which come from the corner bearings and therefore
     * from the pose. So the right question is not "is the successor a function of
     * the span state" -- it is not -- but whether the program factorises: given
     * the columns from outside, is the program determined? */
    {
        static const char *SN[4]={"invd + class + yaw",
                                  "invd + class + yaw + c0",
                                  "invd + class + yaw + c0 + c1",
                                  "invd + class + yaw + c0 + c1 + wall id"};
        int sl;
        printf("C  is the CURRENT program a function of a pose-free state, given the\n");
        printf("   projected columns from outside? If so the rotational machine\n");
        printf("   factorises into an endpoint event stream and a program indexed by\n");
        printf("   (invd, class, columns) sweeping yaw.\n");
        printf("  %-40s %12s %12s %10s\n","state","states","conflicting","by state");
        for(sl=0;sl<4;++sl){
            hs_reset(); cellno=0;
            for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
                int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
                if(!tsp_is_walkable_q4(cx,cy)) continue;
                ++cellno; if((cellno-1u)%cell_stride) continue;
                for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
                    int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
                    int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
                    if(!tsp_is_walkable_q4(px,py)) continue;
                    for(yaw=0;yaw<256u;++yaw){
                        Frame f0; int i;
                        sample(px,py,yaw,&f0);
                        for(i=0;i<f0.n;++i){
                            Obs *a=&f0.o[i]; uint64_t h=1469598103934665603ull;
                            #define MX(v) do{ h^=(uint64_t)(v); h*=1099511628211ull; }while(0)
                            MX(a->invd+1); MX(a->cls+1); MX(yaw+1);
                            if(sl>=1) MX(a->c0+1);
                            if(sl>=2) MX(a->c1+1);
                            if(sl>=3) MX(a->sid+1);
                            #undef MX
                            hs_put(h,a->prog);
                        }
                    }
                }
            }
            printf("  %-40s %12lu %12lu %9.3f%%\n",SN[sl],nstate,nconf,
                   100.0*nconf/(double)(nstate?nstate:1));
        }
        printf("\n");
    }

    /* D. The factorisation above is implied by dp_derive's own signature -- it
     * takes exactly (class, invd, c0, c1, yaw) -- so on its own it confirms the
     * harness rather than discovering anything. What it does establish is the
     * NEGATIVE: nothing else is needed, in particular not the pose, not iq, and
     * not the wall id beyond its normal class. Combined with section A, that
     * says the obstruction to a yaw machine over span state is precisely the
     * projected columns.
     *
     * So: factor the columns out, hold (invd, class, c0, c1) fixed, sweep yaw
     * through a full turn, and ask how many distinct rotational programs exist
     * and how long each holds. If these collapse the way the static trajectories
     * did, a rotational program vocabulary exists. If they do not, rotation
     * really does generate an event almost every tick and the machine must
     * encode transitions cheaply rather than avoid them.
     */
    {
        typedef struct { uint8_t invd,cls,c0,c1; } Key;
        static Key keys[400000]; static int nkeys=0;
        static unsigned char kseen[256*8*20*20];
        unsigned long seqs=0,holds=0,changes=0,hist[33];
        int ki,ci2;
        memset(kseen,0,sizeof kseen);
        memset(hist,0,sizeof hist);
        cellno=0;
        for(gy=0;gy<GRID_H;++gy) for(gx=0;gx<GRID_W;++gx){
            int16_t cx=(int16_t)(gx*CELL_Q4+32),cy=(int16_t)(gy*CELL_Q4+32);
            if(!tsp_is_walkable_q4(cx,cy)) continue;
            ++cellno; if((cellno-1u)%cell_stride) continue;
            for(oy=0;oy<sub;++oy) for(ox=0;ox<sub;++ox){
                int16_t px=(int16_t)(gx*CELL_Q4+(int)(ox*64u/sub));
                int16_t py=(int16_t)(gy*CELL_Q4+(int)(oy*64u/sub));
                if(!tsp_is_walkable_q4(px,py)) continue;
                for(yaw=0;yaw<256u;yaw+=8u){
                    Frame f0; int i;
                    sample(px,py,yaw,&f0);
                    for(i=0;i<f0.n;++i){
                        Obs *a=&f0.o[i];
                        unsigned idx=((unsigned)a->invd*8u+a->cls)*400u+(unsigned)a->c0*20u+a->c1;
                        if(idx<sizeof kseen && !kseen[idx] && nkeys<400000){
                            kseen[idx]=1;
                            keys[nkeys].invd=a->invd; keys[nkeys].cls=a->cls;
                            keys[nkeys].c0=a->c0; keys[nkeys].c1=a->c1; ++nkeys;
                        }
                    }
                }
            }
        }
        hs_reset();
        for(ki=0;ki<nkeys;++ki){
            uint64_t seq=1469598103934665603ull,prev=0; int run=0;
            for(ci2=0;ci2<256;++ci2){
                int8_t nf=k_depth_nf_q7[keys[ki].cls][ci2], sf=k_depth_stepfac_q4[keys[ki].cls][ci2];
                int16_t iq=shr_signed((int16_t)((int16_t)keys[ki].invd*(int16_t)nf),1);
                int16_t st=shr_signed((int16_t)((int16_t)keys[ki].invd*(int16_t)sf),4);
                uint64_t pr; int j2,nch; uint8_t i2,n2=(uint8_t)(keys[ki].c1-keys[ki].c0+1u);
                if(keys[ki].c0<10u){ for(i2=keys[ki].c0;i2<10u;++i2) iq=(int16_t)(iq-st); }
                else { for(i2=10u;i2<keys[ki].c0;++i2) iq=(int16_t)(iq+st); }
                if(iq<0){ iq=(int16_t)-iq; st=(int16_t)-st; }
                pr=1469598103934665603ull; nch=((int)n2+CHUNK-1)/CHUNK;
                for(j2=0;j2<nch;++j2){ pr^=chunk_sig6((int16_t)(iq+(int16_t)(j2*CHUNK*st)),st);
                                       pr*=1099511628211ull; }
                seq^=pr; seq*=1099511628211ull;
                if(ci2&&pr!=prev){ ++changes; if(run<33) ++hist[run]; holds+=(unsigned long)run; run=0; }
                ++run; prev=pr;
            }
            hs_put(seq,0); ++seqs;
        }
        printf("D  rotational programs: (invd, class, c0, c1) held fixed, yaw swept a full turn\n");
        printf("  distinct (invd, class, c0, c1) configurations observed   %d\n",nkeys);
        printf("  distinct 256-yaw program sequences                       %lu\n",nstate);
        printf("  collapse                                                 %.1f : 1\n",
               (double)seqs/(double)(nstate?nstate:1));
        printf("  program changes per full turn, mean                      %.1f of 256\n",
               (double)changes/(double)(seqs?seqs:1));
        printf("  mean yaw units held between changes                      %.2f\n",
               (double)holds/(double)(changes?changes:1));
        printf("  hold-length distribution (yaw units):");
        { int h2; for(h2=1;h2<12;++h2) if(hist[h2])
            printf(" %d:%.1f%%",h2,100.0*hist[h2]/(double)(changes?changes:1)); }
        printf("\n\n");
    }

    printf("\n  'conflicting' = states where two samples carrying the same state produced\n");
    printf("  DIFFERENT successors, so that state is not sufficient to drive a yaw tick.\n");
    printf("  Both +1 and -1 are measured and pooled.\n");
    return 0;
}
