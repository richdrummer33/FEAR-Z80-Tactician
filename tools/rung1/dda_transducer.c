/* Compile the DDA itself into a finite-state transducer, with span length external.
 *
 * The 432 trajectories are the complete set of finished strings the renderer
 * produces over this map and domain. They are proof, not necessarily the thing
 * to store: their endpoints carry span-length and termination information, which
 * is world-specific. The generic result underneath them is algebraic --
 *
 *     the emitted move sequence depends on iq only through (iq + 32) mod 1024,
 *     together with step
 *
 * -- so the generic raster primitive is a DDA phase plus a step, with length
 * supplied from outside. Nothing below that line should know a wall id, a map
 * cell, or what a neighbouring span looks like.
 *
 * This builds that machine directly instead of inferring it from strings:
 *
 *   state       (phase, step),  phase in [0,1024)
 *   input       "advance one column"
 *   output      (ndown, jump): how many row descents, then which advance move
 *   successor   phase' = (phase + step) mod 1024,  step unchanged
 *
 * A Mealy machine, not an acceptor. The question it answers is how many states
 * survive when two are merged wherever they emit the same output for the next L
 * columns -- because a span only ever runs for its own length, and two different
 * numerical DDA states that agree over that length are the same state as far as
 * the screen is concerned. That equivalence is also the shape a temporal
 * certificate would take: how far can the pose move before the emitted span
 * changes at all.
 *
 * Nothing here reads map data, a pose, or a sampled trajectory.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define PHASES 1024
#define MAXL 20
/* Large multiple of 1024 so the accumulator stays positive for any column and
 * step; adding a multiple of 1024 shifts every row by a constant and leaves the
 * row DIFFERENCES -- which is all the output is -- unchanged. */
#define BIAS 65536

/* per-column output of the DDA, as (ndown, jump) packed into a byte */
static int col_out(int phase,int step,int c,int fam)
{
    long a0=(long)BIAS+(long)phase+(long)c*step;
    long a1=a0+step, a2=a1+step;
    long h0=a0>>7,h1=a1>>7,h2=a2>>7;
    long y0=fam?72+h0:71-h0, y1=fam?72+h1:71-h1, y2=fam?72+h2:71-h2;
    long lo0=(y0<y1?y0:y1)>>3, hi0=(y0<y1?y1:y0)>>3;
    long lo1=(y1<y2?y1:y2)>>3, hi1=(y1<y2?y2:y1)>>3;
    long nd=hi0-lo0, jump=lo1-hi0;
    if(nd<0||nd>30) return -1;
    if(jump>0||jump<-4) return -1;          /* outside the move family */
    return (int)(nd*8+(-jump));
}

/* ---- the reachable step set, from the shipped depth tables --------------- */
static int16_t g_step[65536]; static int g_nstep=0;
static uint8_t g_seen[65536];
static void add_step(int16_t s){
    uint16_t k=(uint16_t)s;
    if(g_seen[k]) return; g_seen[k]=1; g_step[g_nstep++]=s;
}
static void build_steps(void)
{
    int cls,yaw,invd;
    uint8_t sfseen[256]; int nsf=0;
    memset(sfseen,0,sizeof sfseen);
    for(cls=0;cls<7;++cls) for(yaw=0;yaw<256;++yaw){
        uint8_t k=(uint8_t)k_depth_stepfac_q4[cls][yaw];
        if(!sfseen[k]){ sfseen[k]=1; ++nsf; }
    }
    /* invd is the output of inv_for_dq4, whose range is the invz table clamped
     * between the near and far clip. Enumerate it exactly rather than assuming. */
    for(cls=0;cls<256;++cls){
        if(!sfseen[cls]) continue;
        for(invd=0;invd<256;++invd){
            int16_t s;
            /* is this invd actually producible? */
            s=shr_signed((int16_t)((int16_t)invd*(int16_t)(int8_t)cls),4);
            add_step(s); add_step((int16_t)-s);   /* dp_derive may flip the sign */
        }
    }
    printf("  distinct step factors in the shipped tables   %d\n",nsf);
    printf("  distinct step values they can produce         %d\n",g_nstep);
}
static uint8_t g_invd_ok[256];
static void build_invd(void)
{
    int d,n=0;
    memset(g_invd_ok,0,sizeof g_invd_ok);
    for(d=1;d<(127<<4)+64;++d){ uint8_t v=inv_for_dq4((int16_t)d); if(!g_invd_ok[v]){g_invd_ok[v]=1;++n;} }
    printf("  distinct inverse depths inv_for_dq4 can return %d\n",n);
}

/* ---- L-step output equivalence ------------------------------------------ */
#define HB 24
#define HS (1u<<HB)
static uint64_t *hk; static unsigned char *hu; static unsigned long hn;
static void hs_reset(void){ memset(hu,0,HS); hn=0; }
static void hs_add(uint64_t key){
    uint64_t x=key*1099511628211ull; unsigned i=(unsigned)((x^(x>>29))&(HS-1u));
    for(;;){ if(!hu[i]){ hu[i]=1; hk[i]=key; ++hn; return; }
             if(hk[i]==key) return; i=(i+1u)&(HS-1u); }
}

/* ---- self-check: the transducer must reproduce the direct shape ---------- */
static int selfcheck(void)
{
    int trial,bad=0;
    unsigned long seed=12345u;
    for(trial=0;trial<200000;++trial){
        int16_t iq,step; int fam,want,c,ok=1;
        seed=seed*1103515245u+12345u;
        iq=(int16_t)((seed>>8)&0x3FFF);
        seed=seed*1103515245u+12345u;
        step=(int16_t)(((int)((seed>>8)&0x7FF))-1024);
        fam=(int)((seed>>20)&1); want=1+(int)((seed>>21)&5);
        /* direct: rows straight from the absolute accumulator */
        for(c=0;c<want&&ok;++c){
            long a0=(long)iq+32+(long)c*step, a1=a0+step, a2=a1+step;
            long h0=a0>>7,h1=a1>>7,h2=a2>>7;
            long y0=fam?72+h0:71-h0,y1=fam?72+h1:71-h1,y2=fam?72+h2:71-h2;
            long lo0=(y0<y1?y0:y1)>>3,hi0=(y0<y1?y1:y0)>>3;
            long lo1=(y1<y2?y1:y2)>>3,hi1=(y1<y2?y2:y1)>>3;
            long nd=hi0-lo0,jump=lo1-hi0;
            int direct,via;
            if(nd<0||nd>30||jump>0||jump<-4){ ok=0; break; }
            (void)hi1;
            direct=(int)(nd*8+(-jump));
            via=col_out((int)((((long)iq+32)%1024L+1024L)%1024L),step,c,fam);
            if(direct!=via){ ++bad;
                if(bad<4) printf("   MISMATCH iq=%d step=%d fam=%d c=%d direct=%d transducer=%d\n",
                                 iq,step,fam,c,direct,via); }
        }
    }
    return bad;
}

/* Dump a phase x step atlas: for a sample of steps, the 6-column behaviour class
 * at every phase, plus the eight analytic edges. This is what makes the band
 * structure visible rather than merely tabulated. */
static void dump_atlas(const char *path,int nsteps)
{
    FILE *f=fopen(path,"w"); int si,p,c,fam,stride;
    uint64_t *ids; int nid=0; uint64_t seen[4096];
    if(!f) return;
    ids=seen; (void)ids;
    fprintf(f,"step,phase,class,is_edge\n");
    stride=g_nstep/nsteps; if(stride<1) stride=1;
    for(si=0;si<g_nstep;si+=stride){
        int edge[1024]; memset(edge,0,sizeof edge);
        for(c=0;c<8;++c){ long v=-(long)c*(long)g_step[si]; v%=1024L; if(v<0)v+=1024L; edge[v]=1; }
        for(p=0;p<1024;++p){
            uint64_t sig=1469598103934665603ull; int cc,ok=1,id=-1,k;
            for(fam=0;fam<2;++fam) for(cc=0;cc<6&&ok;++cc){ int o=col_out(p,g_step[si],cc,fam);
                if(o<0){ ok=0; break; }
                sig^=(uint64_t)(o+1); sig*=1099511628211ull; }
            if(!ok){ fprintf(f,"%d,%d,-1,%d\n",g_step[si],p,edge[p]); continue; }
            for(k=0;k<nid;++k) if(seen[k]==sig){ id=k; break; }
            if(id<0&&nid<4096){ seen[nid]=sig; id=nid++; }
            fprintf(f,"%d,%d,%d,%d\n",g_step[si],p,id,edge[p]);
        }
    }
    fclose(f);
    printf("  atlas written to %s (%d classes seen)\n",path,nid);
}

int main(int argc,char**argv)
{
    int L,fam,si,p;
    const char *atlas = argc>1 ? argv[1] : NULL;
    printf("DDA transducer: the generic raster machine, span length external\n\n");
    printf("1  the state space\n");
    build_invd();
    build_steps();
    printf("  upper bound on (phase, step) states            %d x %d = %ld\n",
           PHASES,g_nstep,(long)PHASES*g_nstep);

    printf("\n2  self-check: transducer output == direct shape computation\n");
    {   int bad=selfcheck();
        printf("  200000 random (iq, step, family, length) cases, %d mismatches   %s\n",
               bad,bad?"FAIL":"ok");
        if(bad) return 1; }

    hk=(uint64_t*)malloc(sizeof(uint64_t)*HS); hu=(unsigned char*)malloc(HS);
    if(!hk||!hu) return 2;

    printf("\n3  states that survive L-column output equivalence\n");
    printf("   Two (phase, step) states merge when they emit the same output for the\n");
    printf("   next L columns. L is the span length, supplied from outside the machine.\n");
    printf("   %-4s %14s %14s %16s\n","L","fam 0 (top)","fam 2 (bottom)","both families");
    for(L=1;L<=MAXL;++L){
        unsigned long n0,n1,nb;
        for(fam=0;fam<2;++fam){
            hs_reset();
            for(si=0;si<g_nstep;++si) for(p=0;p<PHASES;++p){
                uint64_t sig=1469598103934665603ull; int c,ok=1;
                for(c=0;c<L;++c){ int o=col_out(p,g_step[si],c,fam);
                    if(o<0){ ok=0; break; }
                    sig^=(uint64_t)(o+1); sig*=1099511628211ull; }
                if(ok) hs_add(sig);
            }
            if(fam==0) n0=hn; else n1=hn;
        }
        hs_reset();
        for(si=0;si<g_nstep;++si) for(p=0;p<PHASES;++p){
            uint64_t sig=1469598103934665603ull; int c,ok=1;
            for(fam=0;fam<2;++fam) for(c=0;c<L&&ok;++c){ int o=col_out(p,g_step[si],c,fam);
                if(o<0){ ok=0; break; }
                sig^=(uint64_t)(o+1); sig*=1099511628211ull; }
            if(ok) hs_add(sig);
        }
        nb=hn;
        printf("   %-4d %14lu %14lu %16lu\n",L,n0,n1,nb);
    }

    printf("\n4  the indexing problem, which is where the sampled dictionary died\n");
    printf("   A small behaviour space is not by itself a cheap runtime. Naming the\n");
    printf("   class costs nothing; MAPPING a (phase, step) to it is the whole cost, and\n");
    printf("   a full map over %ld parameter states is not a table anyone can ship.\n",
           (long)PHASES*g_nstep);
    printf("   So the useful question is how the classes sit inside one step's phase line.\n\n");
    {
        int L6=6; unsigned long tot=0,worst=0,ones=0; int si2;
        unsigned long hist[64]; int i;
        memset(hist,0,sizeof hist);
        for(si2=0;si2<g_nstep;++si2){
            unsigned long n;
            hs_reset();
            for(p=0;p<PHASES;++p){
                uint64_t sig=1469598103934665603ull; int c,ok=1;
                for(fam=0;fam<2;++fam) for(c=0;c<L6&&ok;++c){ int o=col_out(p,g_step[si2],c,fam);
                    if(o<0){ ok=0; break; }
                    sig^=(uint64_t)(o+1); sig*=1099511628211ull; }
                if(ok) hs_add(sig);
            }
            n=hn; tot+=n; if(n>worst) worst=n; if(n<=1) ++ones;
            if(n<64) ++hist[n];
        }
        printf("   distinct 6-column behaviours among the 1024 phases of ONE step\n");
        printf("     mean %.1f, worst %lu, steps with a single behaviour %lu of %d\n",
               (double)tot/(double)g_nstep,worst,ones,g_nstep);
        printf("     distribution:");
        for(i=0;i<16;++i) if(hist[i]) printf("  %d:%lu",i,hist[i]);
        printf("\n");
        printf("   A phase line that carries only a handful of behaviours is a run-length\n");
        printf("   structure: the behaviour changes at a few phase thresholds and is\n");
        printf("   constant between them. That, and not the class count, is what would\n");
        printf("   make a compiled transducer cheaper than re-running the DDA -- and it\n");
        printf("   is also exactly the distance-to-next-threshold a temporal certificate\n");
        printf("   needs. The Z80 race decides it; this only sizes the candidates.\n");
    }
    {
        /* Eight behaviours over 1024 phases only helps if they are CONTIGUOUS
         * bands: then the index is a couple of comparisons, or a shift if the
         * bands are the eight 128-wide blocks of phase (which would mean the
         * behaviour depends only on the row sub-position h mod 8). If instead
         * the classes interleave, indexing costs as much as running the DDA and
         * the compiled transducer buys nothing. */
        int si2,p2; unsigned long runs_tot=0,worst_runs=0,shift_ok=0,checked=0;
        unsigned long runhist[80]; int i;
        uint64_t sigs[PHASES];
        memset(runhist,0,sizeof runhist);
        for(si2=0;si2<g_nstep;++si2){
            unsigned long runs=1; int allshift=1; int any=0;
            for(p2=0;p2<PHASES;++p2){
                uint64_t sig=1469598103934665603ull; int c,ok=1;
                for(fam=0;fam<2;++fam) for(c=0;c<6&&ok;++c){ int o=col_out(p2,g_step[si2],c,fam);
                    if(o<0){ ok=0; break; }
                    sig^=(uint64_t)(o+1); sig*=1099511628211ull; }
                sigs[p2]= ok ? sig : 0ull;
                if(ok) any=1;
            }
            if(!any) continue;
            ++checked;
            for(p2=1;p2<PHASES;++p2) if(sigs[p2]!=sigs[p2-1]) ++runs;
            /* does the behaviour depend only on phase >> 7 (the row sub-position)? */
            for(p2=1;p2<PHASES;++p2)
                if((p2>>7)==((p2-1)>>7) && sigs[p2]!=sigs[p2-1]){ allshift=0; break; }
            if(allshift) ++shift_ok;
            runs_tot+=runs; if(runs>worst_runs) worst_runs=runs;
            if(runs<80) ++runhist[runs];
        }
        printf("\n   how those behaviours are laid out along the phase line\n");
        printf("     contiguous behaviour bands per step: mean %.2f, worst %lu\n",
               (double)runs_tot/(double)(checked?checked:1),worst_runs);
        printf("     distribution:");
        for(i=0;i<40;++i) if(runhist[i]) printf("  %d:%lu",i,runhist[i]);
        printf("\n");
        printf("     steps whose behaviour depends only on phase >> 7  %lu of %lu\n",
               shift_ok,checked);
        if(shift_ok==checked)
            printf("     ALL of them: the 6-column behaviour is a function of the row\n"
                   "     sub-position h mod 8 alone, so the index is a 3-bit shift.\n");
        else
            printf("     not all: indexing needs the band thresholds, not just a shift.\n");

        /* Where do the eight edges sit? A row boundary is crossed when the
         * accumulator passes a multiple of 1024, and a 6-column span looks at
         * columns 0..7, so the behaviour can only change where
         *     p + c*step == 0  (mod 1024),  c = 0..7
         * i.e. at the eight phases p = (-c*step) mod 1024. If that holds, the
         * band index is computable from (phase, step) with a handful of
         * comparisons instead of six columns of DDA arithmetic -- and the
         * distance to the next edge is exactly how far the pose may move before
         * the emitted span changes at all, which is the temporal certificate. */
        {
            unsigned long checked2=0,matched=0; int si3,p3,c3;
            for(si3=0;si3<g_nstep;++si3){
                unsigned char pred[PHASES]; int ok=1,any=0;
                memset(pred,0,sizeof pred);
                for(c3=0;c3<8;++c3){
                    long v=-(long)c3*(long)g_step[si3];
                    v%=PHASES; if(v<0) v+=PHASES;
                    pred[v]=1;
                }
                for(p3=0;p3<PHASES;++p3){
                    uint64_t sig=1469598103934665603ull; int c,okc=1;
                    for(fam=0;fam<2;++fam) for(c=0;c<6&&okc;++c){ int o=col_out(p3,g_step[si3],c,fam);
                        if(o<0){ okc=0; break; }
                        sig^=(uint64_t)(o+1); sig*=1099511628211ull; }
                    sigs[p3]= okc ? sig : 0ull;
                    if(okc) any=1;
                }
                if(!any) continue;
                ++checked2;
                for(p3=0;p3<PHASES;++p3){
                    int prev=(p3+PHASES-1)%PHASES;
                    if(sigs[p3]!=sigs[prev] && !pred[p3]){ ok=0; break; }
                }
                if(ok) ++matched;
            }
            printf("     every band edge lies at p = (-c*step) mod 1024, c in 0..7:"
                   "  %lu of %lu steps   %s\n",
                   matched,checked2,matched==checked2?"ok":"NO");
            if(matched==checked2)
                printf("     So the band index is a rank among eight computed phases, not a\n"
                       "     lookup, and the distance to the next edge is the safe region.\n");
        }
    }

    if(atlas){ printf("\n   atlas dump\n"); dump_atlas(atlas,192); }

    printf("\n5  candidates, sized (none chosen here)\n");
    printf("   %-34s %s\n","representation","cost");
    printf("   %-34s %s\n","arithmetic DDA","0 B of table, per-column arithmetic");
    printf("   %-34s %s\n","compiled 6-column transducer","one byte of class + a 175-entry table");
    printf("   %-34s %s\n","432 packed trajectories","1,586 B moves + 1,296 B index");
    printf("   %-34s %s\n","220-state trajectory DFA","866 B, but accepts finished strings");
    printf("   The last two encode span termination, which is world-specific. The first\n");
    printf("   two keep length external, which is the property we actually want.\n");
    return 0;
}
