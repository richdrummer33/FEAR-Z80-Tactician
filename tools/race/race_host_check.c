/* Host checker for the race kernels: same header the ROM compiles, so a
 * correctness bug costs a second here instead of a build-and-emulate cycle. */
#include <stdio.h>
#include <string.h>
#include "race_kernels.h"

int main(void)
{
    uint8_t a[MAXMV],b[MAXMV],c[MAXMV];
    int li,ci,k,bad=0,shown=0;
    for(li=0;li<RACE_NLEN;++li) for(ci=0;ci<RACE_NCASE;++ci){
        int idx=li*RACE_NCASE+ci, want=k_race_mlen[idx];
        const uint8_t *exp=&k_race_blob[k_race_off[idx]];
        uint8_t na,nb,nc; int mism=0;
        na=dda_span(k_race_iq[ci],k_race_step[ci],k_race_len[li],a);
        band_setup(k_race_step[ci]);
        nb=band_span(k_race_iq[ci],k_race_step[ci],k_race_len[li],b);
        nc=pack_span(exp,(uint8_t)want,c);
        if(na!=want||nb!=want||nc!=want) mism=1;
        else for(k=0;k<want;++k) if(a[k]!=exp[k]||b[k]!=exp[k]||c[k]!=exp[k]) mism=1;
        if(mism){ ++bad;
            if(shown<4){ ++shown;
                printf("case %d len %d step %d: want %d dda %d band %d\n",
                       ci,k_race_len[li],k_race_step[ci],want,na,nb);
                printf("  want:"); for(k=0;k<want;++k) printf(" %d",exp[k]); printf("\n");
                printf("  dda :"); for(k=0;k<na;++k) printf(" %d",a[k]); printf("\n");
                printf("  band:"); for(k=0;k<nb;++k) printf(" %d",b[k]); printf("\n"); } }
    }
    printf("%d of %d cases disagree\n",bad,RACE_NLEN*RACE_NCASE);
    return bad?1:0;
}
