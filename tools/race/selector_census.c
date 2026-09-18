/* Census for the selector ladder: band distribution, key ambiguity structure,
 * and cheap residual tests.
 *
 * The earlier screening reported keys as sufficient or insufficient. That is a
 * good first filter and a bad stopping point. A key that leaves two candidate
 * bodies which one BIT instruction separates is an excellent first stage; a key
 * that leaves two candidates needing a 16-bit geometric quantity rebuilt is not.
 * Both are labelled "insufficient". So this measures the STRUCTURE of the
 * failure, not just its existence.
 *
 * It also censuses the phase bands properly. "About eight per step" is an
 * average, and an average of eight made of mostly six plus a few pathological
 * thirties is a different engineering problem from a tight distribution. The
 * whole interval-selector hypothesis rests on which of those it is.
 *
 * Bands here are MERGED contiguous phase intervals: if two adjacent intervals
 * resolve to the same body they are one band, because a selector would never
 * need to distinguish them.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define BIAS 65536
#define NPH 1024
#define INVALID 0xFFFF

static int col_out(int phase,int step,int c,int fam)
{
    long a0=(long)BIAS+(long)phase+(long)c*step,a1=a0+step,a2=a1+step;
    long h0=a0>>7,h1=a1>>7,h2=a2>>7;
    long y0=fam?72+h0:71-h0,y1=fam?72+h1:71-h1,y2=fam?72+h2:71-h2;
    long lo0=(y0<y1?y0:y1)>>3,hi0=(y0<y1?y1:y0)>>3;
    long lo1=(y1<y2?y1:y2)>>3;
    long nd=hi0-lo0,jump=lo1-hi0;
    if(nd<0||nd>30||jump>0||jump<-4) return -1;
    return (int)(nd*8+(-jump));
}
static int16_t g_step[8192]; static int g_nstep=0;
static void build_steps(void)
{
    static unsigned char seen[65536],sf_seen[256];
    int cls,yaw,invd,sf,v;
    for(cls=0;cls<7;++cls) for(yaw=0;yaw<256;++yaw) sf_seen[(uint8_t)k_depth_stepfac_q4[cls][yaw]]=1;
    for(sf=0;sf<256;++sf){ if(!sf_seen[sf]) continue;
        for(invd=0;invd<256;++invd){
            int16_t s=shr_signed((int16_t)((int16_t)invd*(int16_t)(int8_t)sf),4);
            for(v=0;v<2;++v){ int16_t x=v?(int16_t)-s:s;
                if(!seen[(uint16_t)x]&&g_nstep<8192){ seen[(uint16_t)x]=1; g_step[g_nstep++]=x; } } } }
}
static uint16_t *g_beh; static int g_nbeh=0;
static void build_beh(void)
{
    static uint64_t sigs[8192]; int nsig=0,si,ph;
    g_beh=(uint16_t*)malloc(sizeof(uint16_t)*(size_t)g_nstep*NPH);
    for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
        uint64_t sg=1469598103934665603ull; int fam,c,ok=1,k;
        for(fam=0;fam<2;++fam) for(c=0;c<6&&ok;++c){ int o=col_out(ph,g_step[si],c,fam);
            if(o<0){ ok=0; break; } sg^=(uint64_t)(o+1); sg*=1099511628211ull; }
        if(!ok){ g_beh[(size_t)si*NPH+ph]=INVALID; continue; }
        for(k=0;k<nsig;++k) if(sigs[k]==sg) break;
        if(k==nsig&&nsig<8192) sigs[nsig++]=sg;
        g_beh[(size_t)si*NPH+ph]=(uint16_t)k; }
    g_nbeh=nsig;
}
static int cmp_int(const void*a,const void*b){ return *(const int*)a-*(const int*)b; }
static double pct(int *sorted,int n,double q){ int i=(int)(q*(n-1)); return sorted[i]; }

/* ---- key candidates ------------------------------------------------------ */
static int g_order[8192],g_norder=0;
static void build_orders(void)
{
    static int okey[8192]; int n=0,si,i,j;
    for(si=0;si<g_nstep;++si){
        int ph[8],ord[8],key=0;
        for(i=0;i<8;++i){ long v=-(long)i*(long)g_step[si]; v%=1024L; if(v<0)v+=1024L; ph[i]=(int)v; ord[i]=i; }
        for(i=0;i<8;++i) for(j=i+1;j<8;++j) if(ph[ord[j]]<ph[ord[i]]){ int t=ord[i];ord[i]=ord[j];ord[j]=t; }
        for(i=0;i<8;++i) key=key*8+ord[i];
        for(i=0;i<n;++i) if(okey[i]==key) break;
        if(i==n&&n<8192) okey[n++]=key;
        g_order[si]=i; }
    g_norder=n;
}
static int band_of(int step,int phase)
{ int c,r=0; for(c=0;c<8;++c){ long v=-(long)c*(long)step; v%=1024L; if(v<0)v+=1024L; if(v<=phase) ++r; } return r-1; }

static uint32_t key_of(int which,int si,int ph)
{
    switch(which){
    case 0: return ((uint32_t)g_order[si]<<4)|(uint32_t)(band_of(g_step[si],ph)&15);
    case 1: return ((uint32_t)g_order[si]<<10)|(uint32_t)ph;
    case 2: return ((uint32_t)((uint16_t)g_step[si]>>8)<<10)|(uint32_t)ph;
    case 3: return ((uint32_t)((uint16_t)g_step[si]>>4)<<10)|(uint32_t)ph;
    default: return 0;
    }
}
static const char *KNAME[4]={"edge-ordering family + band",
                             "edge-ordering family + full phase",
                             "step high byte + full phase",
                             "step >> 4 + full phase"};

/* permitted cheap residual predicates: individual bits, and the step sign */
static int pred_of(int p,int si,int ph)
{
    if(p<10) return (ph>>p)&1;
    if(p<18) return (((uint16_t)g_step[si])>>(p-10))&1;
    return g_step[si]<0;
}
static const int NPRED=19;

int main(int argc,char**argv)
{
    const char *od=argc>1?argv[1]:"build/selector";
    char path[512]; FILE *f;
    int si,ph,i;
    build_steps(); build_orders(); build_beh();
    printf("selector census\n  %d reachable steps x %d phases = %d states, %d bodies, %d orderings\n\n",
           g_nstep,NPH,g_nstep*NPH,g_nbeh,g_norder);

    /* ---- 1. merged phase bands per exact step ---- */
    {
        int *nb=(int*)malloc(sizeof(int)*g_nstep);
        int *sorted=(int*)malloc(sizeof(int)*g_nstep);
        long tot=0; int maxb=0,hist[64]; long ninv=0;
        memset(hist,0,sizeof hist);
        snprintf(path,sizeof path,"%s/bands_per_step.csv",od); f=fopen(path,"w");
        if(f) fprintf(f,"ordinal,step,bands,invalid_phases\n");
        for(si=0;si<g_nstep;++si){
            int runs=0,inv=0; uint16_t prev=0xFFFE;
            for(ph=0;ph<NPH;++ph){
                uint16_t b=g_beh[(size_t)si*NPH+ph];
                if(b==INVALID){ ++inv; }
                if(b!=prev){ ++runs; prev=b; }
            }
            nb[si]=runs; tot+=runs; ninv+=inv;
            if(runs>maxb) maxb=runs;
            if(runs<64) ++hist[runs];
            if(f) fprintf(f,"%d,%d,%d,%d\n",si,g_step[si],runs,inv);
        }
        if(f) fclose(f);
        memcpy(sorted,nb,sizeof(int)*g_nstep); qsort(sorted,g_nstep,sizeof(int),cmp_int);
        printf("1  merged contiguous phase bands per exact step\n");
        printf("   mean %.2f  median %.0f  p90 %.0f  p95 %.0f  p99 %.0f  max %d\n",
               (double)tot/g_nstep,pct(sorted,g_nstep,0.50),pct(sorted,g_nstep,0.90),
               pct(sorted,g_nstep,0.95),pct(sorted,g_nstep,0.99),maxb);
        printf("   histogram:");
        for(i=0;i<40;++i) if(hist[i]) printf("  %d:%d",i,hist[i]);
        printf("\n   phases with no in-family body: %ld of %d (%.3f%%)\n",
               ninv,g_nstep*NPH,100.0*ninv/(g_nstep*NPH));
        printf("   total band records if every step is stored independently: %ld\n",tot);
        printf("   at 1 threshold byte-pair + 1 body byte per band: ~%ld bytes\n\n",tot*3);
        free(nb); free(sorted);
    }

    /* ---- 1b. the exact body map, as raw bytes for the structural plot ---- */
    {
        snprintf(path,sizeof path,"%s/body_map.u8",od); f=fopen(path,"wb");
        if(f){ unsigned char *row=(unsigned char*)malloc(NPH);
               for(si=0;si<g_nstep;++si){
                   for(ph=0;ph<NPH;++ph){ uint16_t b=g_beh[(size_t)si*NPH+ph];
                       row[ph]=(unsigned char)(b==INVALID?255:b); }
                   fwrite(row,1,NPH,f); }
               free(row); fclose(f);
               printf("   body map written: %d x %d bytes (255 = no in-family body)\n\n",g_nstep,NPH); }
        snprintf(path,sizeof path,"%s/step_ordinals.csv",od); f=fopen(path,"w");
        if(f){ fprintf(f,"ordinal,step\n");
               for(si=0;si<g_nstep;++si) fprintf(f,"%d,%d\n",si,g_step[si]); fclose(f); }
    }

    /* ---- 2. body occupancy ---- */
    {
        long *occ=(long*)calloc(g_nbeh,sizeof(long)); long tot=0;
        snprintf(path,sizeof path,"%s/body_occupancy.csv",od); f=fopen(path,"w");
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint16_t b=g_beh[(size_t)si*NPH+ph];
            if(b!=INVALID){ ++occ[b]; ++tot; } }
        if(f){ fprintf(f,"body,states\n");
               for(i=0;i<g_nbeh;++i) fprintf(f,"%d,%ld\n",i,occ[i]); fclose(f); }
        { long *s=(long*)malloc(sizeof(long)*g_nbeh); long acc=0; int k;
          memcpy(s,occ,sizeof(long)*g_nbeh);
          for(i=0;i<g_nbeh;++i) for(k=i+1;k<g_nbeh;++k) if(s[k]>s[i]){ long t=s[i];s[i]=s[k];s[k]=t; }
          printf("2  body occupancy over %ld in-family states\n",tot);
          printf("   cumulative share of the top");
          { int tops[6]={1,5,10,25,50,100}; int j;
            for(j=0;j<6;++j){ acc=0; for(i=0;i<tops[j]&&i<g_nbeh;++i) acc+=s[i];
                printf("  %d:%.1f%%",tops[j],100.0*acc/tot); } }
          printf("\n\n"); free(s); }
        free(occ);
    }

    /* ---- 3. ambiguity structure of the failed keys ---- */
    printf("3  residual ambiguity after a coarse key, and whether one cheap bit fixes it\n");
    printf("   %-36s %9s %9s %9s %9s %9s\n","key","keyvals","uniq keys","uniq states","mean cand","max cand");
    snprintf(path,sizeof path,"%s/key_ambiguity.csv",od); f=fopen(path,"w");
    if(f) fprintf(f,"key,candidates,keyvals,states\n");
    for(i=0;i<4;++i){
        /* bitset of bodies per key value */
        uint32_t maxk=0; size_t nk;
        uint32_t *bs; int *cand; long *stcount;
        int words=(g_nbeh+31)/32;
        int j; long uniq_states=0,tot_states=0; int uniq_keys=0,maxc=0; double meanc=0;
        long d1_resolved=0;
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint32_t k=key_of(i,si,ph); if(k>maxk) maxk=k; }
        nk=(size_t)maxk+1;
        bs=(uint32_t*)calloc(nk*words,sizeof(uint32_t));
        cand=(int*)calloc(nk,sizeof(int));
        stcount=(long*)calloc(nk,sizeof(long));
        if(!bs||!cand||!stcount){ printf("   %-36s (out of memory)\n",KNAME[i]); continue; }
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint16_t b=g_beh[(size_t)si*NPH+ph]; uint32_t k;
            if(b==INVALID) continue;
            k=key_of(i,si,ph);
            bs[(size_t)k*words+(b>>5)]|=1u<<(b&31); ++stcount[k]; ++tot_states; }
        { long used=0;
          for(j=0;j<(int)nk;++j){ int c=0,w;
            for(w=0;w<words;++w){ uint32_t x=bs[(size_t)j*words+w]; while(x){ x&=x-1; ++c; } }
            cand[j]=c; if(c){ ++used; meanc+=c; if(c>maxc) maxc=c;
                              if(c==1){ ++uniq_keys; uniq_states+=stcount[j]; } }
            if(f&&c) fprintf(f,"%s,%d,1,%ld\n",KNAME[i],c,stcount[j]); }
          meanc/= (used?used:1);
          printf("   %-36s %9ld %9d %8.2f%% %9.2f %9d\n",KNAME[i],used,uniq_keys,
                 100.0*uniq_states/(tot_states?tot_states:1),meanc,maxc); }
        /* depth-1 residual: is there ONE permitted bit that splits every ambiguous key? */
        {
            uint32_t *sp=(uint32_t*)calloc(nk*(size_t)NPRED*2*words,sizeof(uint32_t));
            if(sp){
                int p;
                for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
                    uint16_t b=g_beh[(size_t)si*NPH+ph]; uint32_t k;
                    if(b==INVALID) continue;
                    k=key_of(i,si,ph);
                    if(cand[k]<2) continue;
                    for(p=0;p<NPRED;++p){
                        size_t off=(((size_t)k*NPRED+p)*2+(size_t)pred_of(p,si,ph))*words;
                        sp[off+(b>>5)]|=1u<<(b&31); } }
                for(j=0;j<(int)nk;++j){
                    if(cand[j]<2) continue;
                    for(p=0;p<NPRED;++p){
                        int ok=1,side,w;
                        for(side=0;side<2&&ok;++side){ int c=0;
                            for(w=0;w<words;++w){ uint32_t x=sp[(((size_t)j*NPRED+p)*2+side)*words+w];
                                while(x){ x&=x-1; ++c; } }
                            if(c>1) ok=0; }
                        if(ok){ d1_resolved+=stcount[j]; break; } } }
                free(sp);
                printf("   %-36s one cheap bit resolves a further %.2f%% of states -> %.2f%% total\n",
                       "", 100.0*d1_resolved/(tot_states?tot_states:1),
                       100.0*(uniq_states+d1_resolved)/(tot_states?tot_states:1));
            }
        }
        free(bs); free(cand); free(stcount);
    }
    if(f) fclose(f);
    printf("\n   permitted residual predicates: the ten phase bits, the eight low step\n");
    printf("   bits, and the step sign -- all single BIT tests on the Z80.\n");
    return 0;
}
