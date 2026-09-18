/* Which cheap keys are SUFFICIENT to name a generic raster body, and how big is
 * the table over the complete domain?
 *
 * No assembly is needed to settle this. A selector is only interesting if its
 * key both (a) determines the body and (b) indexes a table that does not scale
 * with sampled poses. Both are host questions, and answering them first avoids
 * writing Z80 for a key that cannot work.
 *
 * The domain is the COMPLETE one, not a benchmark corpus: all 1024 DDA phases
 * against every step the shipped depth tables can produce. Assuming every phase
 * is reachable for every step overstates the table, which is the safe direction
 * for a ROM-scaling claim.
 *
 * The trap being tested for is BAND's: a key that requires most of the DDA to
 * compute is a solver wearing a lookup's clothes. So each candidate is annotated
 * with what the renderer must do to FORM it, split into what is already live
 * where raster generation begins (iq, step, c0, c1 -- and phase, which is one
 * mask away from iq) and what would have to be reconstructed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define BIAS 65536
#define NPH 1024

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

/* every step the shipped depth tables can produce */
static int16_t g_step[8192]; static int g_nstep=0;
static void build_steps(void)
{
    static unsigned char seen[65536];
    static unsigned char sf_seen[256];
    int cls,yaw,invd,sf;
    for(cls=0;cls<7;++cls) for(yaw=0;yaw<256;++yaw)
        sf_seen[(uint8_t)k_depth_stepfac_q4[cls][yaw]]=1;
    for(sf=0;sf<256;++sf){ if(!sf_seen[sf]) continue;
        for(invd=0;invd<256;++invd){
            int16_t s=shr_signed((int16_t)((int16_t)invd*(int16_t)(int8_t)sf),4);
            int16_t v;
            for(v=0;v<2;++v){ int16_t x=v?(int16_t)-s:s;
                if(!seen[(uint16_t)x]&&g_nstep<8192){ seen[(uint16_t)x]=1; g_step[g_nstep++]=x; } } } }
}

/* the 6-column behaviour id for every (step index, phase) */
static uint16_t *g_beh;      /* 0 = out of family, else id+1 */
static int g_nbeh=0;

/* the edge-ordering family of a step: which permutation the eight edges
   p = (-c*step) mod 1024 appear in along the phase line */
static int16_t g_order[8192]; static int g_norder=0;
static void build_orders(void)
{
    static int okey[4096]; static int oval[4096]; int n=0,si;
    for(si=0;si<g_nstep;++si){
        int ph[8],ord[8],i,j,key=0;
        for(i=0;i<8;++i){ long v=-(long)i*(long)g_step[si]; v%=1024L; if(v<0)v+=1024L;
                          ph[i]=(int)v; ord[i]=i; }
        for(i=0;i<8;++i) for(j=i+1;j<8;++j)
            if(ph[ord[j]]<ph[ord[i]]){ int t=ord[i]; ord[i]=ord[j]; ord[j]=t; }
        for(i=0;i<8;++i) key=key*8+ord[i];
        for(i=0;i<n;++i) if(okey[i]==key) break;
        if(i==n&&n<4096){ okey[n]=key; oval[n]=n; ++n; }
        g_order[si]=(int16_t)i;
    }
    g_norder=n;
}
/* band index: edges at or below the phase, minus one (the c=0 edge is at 0) */
static int band_of(int step,int phase)
{
    int c,r=0;
    for(c=0;c<8;++c){ long v=-(long)c*(long)step; v%=1024L; if(v<0)v+=1024L;
                      if(v<=phase) ++r; }
    return r-1;
}

/* ---- key sufficiency and table size ------------------------------------ */
#define HB 24
#define HS (1u<<HB)
static uint64_t *hk; static uint32_t *hv; static unsigned char *hu;
static unsigned long nkey,nconf;
static void hreset(void){ memset(hu,0,HS); nkey=nconf=0; }
static void hput(uint64_t k,uint32_t v)
{
    uint64_t x=k*1099511628211ull; unsigned i=(unsigned)((x^(x>>29))&(HS-1u));
    for(;;){ if(!hu[i]){ hu[i]=1; hk[i]=k; hv[i]=v; ++nkey; return; }
             if(hk[i]==k){ if(hv[i]!=v) ++nconf; return; }
             i=(i+1u)&(HS-1u); } }

typedef struct { const char *name; const char *forms; int live; } Cand;

int main(void)
{
    int si,ph,pshift,sshift,ci;
    build_steps();
    build_orders();
    printf("selector key screening\n");
    printf("  domain: %d reachable steps x %d phases = %d states\n",
           g_nstep,NPH,g_nstep*NPH);
    printf("  distinct edge-ordering families: %d\n\n",g_norder);

    g_beh=(uint16_t*)malloc(sizeof(uint16_t)*(size_t)g_nstep*NPH);
    hk=(uint64_t*)malloc(sizeof(uint64_t)*HS); hv=(uint32_t*)malloc(sizeof(uint32_t)*HS);
    hu=(unsigned char*)malloc(HS);
    if(!g_beh||!hk||!hv||!hu) return 2;
    /* behaviour id table, one pass */
    hreset();
    { static uint64_t sigs[8192]; int nsig=0;
      for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
        uint64_t sg=1469598103934665603ull; int fam,c,ok=1,k;
        for(fam=0;fam<2;++fam) for(c=0;c<6&&ok;++c){ int o=col_out(ph,g_step[si],c,fam);
            if(o<0){ ok=0; break; } sg^=(uint64_t)(o+1); sg*=1099511628211ull; }
        if(!ok){ g_beh[(size_t)si*NPH+ph]=0; continue; }
        for(k=0;k<nsig;++k) if(sigs[k]==sg) break;
        if(k==nsig&&nsig<8192){ sigs[nsig++]=sg; }
        g_beh[(size_t)si*NPH+ph]=(uint16_t)(k+1); }
      g_nbeh=nsig; }
    printf("  distinct 6-column behaviours over the whole domain: %d\n\n",g_nbeh);

    printf("A  quantised (phase, step): how coarse can the key be and stay sufficient?\n");
    printf("   entries are the table size; a dash means the key is NOT sufficient\n");
    printf("   %-10s","phase>>");
    for(pshift=0;pshift<=6;++pshift) printf(" %9d",pshift);
    printf("\n");
    for(sshift=0;sshift<=6;++sshift){
        printf("   step>>%-4d",sshift);
        for(pshift=0;pshift<=6;++pshift){
            hreset();
            for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
                uint16_t b=g_beh[(size_t)si*NPH+ph];
                if(!b) continue;
                hput(((uint64_t)((uint16_t)g_step[si]>>sshift)<<16)|(uint64_t)(ph>>pshift),b);
            }
            if(nconf) printf(" %9s","-");
            else printf(" %9lu",nkey);
        }
        printf("\n");
    }
    printf("   (sufficient means every state sharing the key emits the same body)\n\n");

    printf("B  structured keys\n");
    printf("   %-42s %10s %12s %s\n","key","entries","sufficient","what must be formed");
    {
        /* B1: (step, band) -- BAND's own key */
        hreset();
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint16_t b=g_beh[(size_t)si*NPH+ph]; if(!b) continue;
            hput(((uint64_t)(uint16_t)g_step[si]<<8)|(uint64_t)band_of(g_step[si],ph),b); }
        printf("   %-42s %10lu %12s %s\n","step + band index",nkey,nconf?"NO":"yes",
               "8 edges + 8 compares: BAND's cost");
        /* B2: (ordering family, band) */
        hreset();
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint16_t b=g_beh[(size_t)si*NPH+ph]; if(!b) continue;
            hput(((uint64_t)(uint16_t)g_order[si]<<8)|(uint64_t)band_of(g_step[si],ph),b); }
        printf("   %-42s %10lu %12s %s\n","edge-ordering family + band index",nkey,nconf?"NO":"yes",
               "family is bakeable; band still costs 8 compares");
        /* B3: (ordering family, phase) */
        hreset();
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint16_t b=g_beh[(size_t)si*NPH+ph]; if(!b) continue;
            hput(((uint64_t)(uint16_t)g_order[si]<<16)|(uint64_t)ph,b); }
        printf("   %-42s %10lu %12s %s\n","edge-ordering family + full phase",nkey,nconf?"NO":"yes",
               "family bakeable, phase already live");
        /* B4: (step high byte, phase) */
        hreset();
        for(si=0;si<g_nstep;++si) for(ph=0;ph<NPH;++ph){
            uint16_t b=g_beh[(size_t)si*NPH+ph]; if(!b) continue;
            hput(((uint64_t)((uint16_t)g_step[si]>>8)<<16)|(uint64_t)ph,b); }
        printf("   %-42s %10lu %12s %s\n","step high byte + full phase",nkey,nconf?"NO":"yes",
               "both already live, one shift");
    }
    printf("\n   ROM at 2 bytes per entry is twice the entry count; a body pointer is\n");
    printf("   2 bytes, so a sufficient key with N entries costs 2N bytes of index.\n");
    return 0;
}
