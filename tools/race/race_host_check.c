/* Host checker for the race kernels: same header the ROM compiles, so a
 * correctness bug costs a second here instead of a build-and-emulate cycle.
 *
 * It also checks the selector ladder, including the step-to-ordinal map A1 has
 * to walk. A selector that names the wrong body produces a plausible-looking
 * raster, so the only useful gate is byte equality against the generated
 * expectation. */
#include <stdio.h>
#include <string.h>
#include "race_kernels.h"

static const char *MODE[4] = {"A0 exact-state", "B fixed, linear",
                              "B fixed, balanced", "B packed, linear"};

int main(void)
{
    uint8_t a[MAXMV],b[MAXMV],c[MAXMV],s[MAXMV];
    int li,ci,k,m,bad=0,shown=0;
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
        /* the ordinal A1 derives must be the one A0 is handed */
        if(sel_a1_ord(k_race_step[ci])!=(uint16_t)ci){
            printf("A1 ordinal for step %d: got %u want %d\n",
                   k_race_step[ci],(unsigned)sel_a1_ord(k_race_step[ci]),ci);
            mism=1; }
        for(m=0;m<4;++m){
            uint8_t ns=sel_span(m,(uint8_t)ci,k_race_iq[ci],k_race_step[ci],k_race_len[li],s);
            int j,wrong=0;
            if(ns!=want) wrong=1;
            else for(j=0;j<want;++j) if(s[j]!=exp[j]) wrong=1;
            if(wrong){ mism=1;
                if(shown<4){ ++shown;
                    printf("%s case %d len %d step %d: want %d got %d\n",
                           MODE[m],ci,k_race_len[li],k_race_step[ci],want,ns);
                    printf("  want:"); for(j=0;j<want;++j) printf(" %d",exp[j]); printf("\n");
                    printf("  got :"); for(j=0;j<ns;++j) printf(" %d",s[j]); printf("\n"); } } }
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
