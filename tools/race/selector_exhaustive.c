/* Exhaustive validation of the production selector over the whole step domain.
 *
 * The race proved eleven kernels agree on 22 steps at four span lengths. That is
 * not the same as proving the baked tables are right: 22 of 3,103 steps is 0.7%
 * of the domain, and the phases those cases happen to visit are a tiny slice of
 * the 1,024 available. This closes that gap on the host, where a full sweep costs
 * seconds rather than a build-and-emulate cycle.
 *
 * The reference is deliberately NOT the race kernels. It is the closed-form
 * column rule written out in wide integers, the same rule the Python generator
 * uses, so a shared 16-bit mistake cannot hide inside both sides of the
 * comparison.
 *
 * Three things are checked:
 *
 *   1. Every step, every phase, a spread of span lengths that covers every
 *      terminal-chunk residue and multi-chunk chaining.
 *   2. Translation invariance -- the premise the whole selector rests on. The
 *      body is looked up by phase alone, so the same phase at different
 *      accumulator offsets must produce the same moves. If that were false the
 *      selector would be wrong in a way no single-offset sweep could see.
 *   3. The thirteen exceptional multiples of 256, and the steps whose records
 *      sit at a cartridge bank boundary, exercised at every phase.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "selfull_hdr.h"
#include "selfull_steps.h"

#define CHUNK 6
#define MAXMV 256

static const uint8_t *RECBANK[SELFULL_NBANK] = {
    gg_selfull_rec0, gg_selfull_rec1, gg_selfull_rec2, gg_selfull_rec3, gg_selfull_rec4
};

/* ---- the reference: the closed-form column rule, in wide integers --------- */
static int ref_span(long a0, long step, int ncols, uint8_t *out)
{
    int n = 0, c;
    for (c = 0; c < ncols; ++c) {
        long p0 = a0 + (long)c * step, p1 = p0 + step, p2 = p1 + step;
        long y0 = 71 - (p0 >> 7), y1 = 71 - (p1 >> 7), y2 = 71 - (p2 >> 7);
        long lo0 = (y0 < y1 ? y0 : y1) >> 3, hi0 = (y0 < y1 ? y1 : y0) >> 3;
        long lo1 = (y1 < y2 ? y1 : y2) >> 3;
        long nd = hi0 - lo0, jump = lo1 - hi0, d;
        if (nd < 0 || nd > 30) return -1;
        for (d = 0; d < nd; ++d) { if (n >= MAXMV) return -1; out[n++] = 0; }
        if (c + 1 == ncols) { out[n++] = 6; return n; }
        if (jump > 0 || jump < -4) return -1;
        out[n++] = (uint8_t)(1 - jump);
    }
    return n;
}

/* ---- the production selector, exactly as the Z80 kernel walks it ---------- */
static int g_mutate;   /* mutation control; zero for every real sweep */
static const uint8_t *rec_of(int16_t step)
{
    uint8_t pg = gg_selfull_hi[(uint16_t)step >> 8];
    const uint8_t *e;
    if (pg == 0xFF) return NULL;
    e = gg_selfull_map + (size_t)pg * 768u + (size_t)((uint16_t)step & 255u) * 3u;
    return RECBANK[g_mutate == 3 ? (e[0] & 3u) : e[0]] + ((size_t)e[1] | ((size_t)e[2] << 8));
}
/* Mutation control. A sweep that cannot fail proves nothing, so each of these
 * is a defect the selector could plausibly have, injected one at a time; the
 * harness asserts every one is caught. They are the realistic failure modes:
 * an off-by-one in the threshold compare, the wrong prefix column, and a bank
 * bit dropped from the map entry. */
static uint8_t body_of(const uint8_t *r, uint16_t ph)
{
    uint8_t phh = (uint8_t)(ph >> 8), phl = (uint8_t)ph;
    int i;
    /* The real scan terminates by construction: the last slot's threshold is
     * zero and the phase is never negative. The bound exists only so an injected
     * defect reports a mismatch instead of walking off the bank and crashing the
     * harness -- a crash is a weaker signal than a wrong answer. */
    for (i = 0; i < 8; ++i, r += 3) {
        if (r[0] < phh) return r[2];
        if (r[0] == phh && (g_mutate == 1 ? r[1] < phl : r[1] <= phl)) return r[2];
    }
    return r[-3 * 8 + 2];
}
static int sel_span(int16_t step, long a0, int ncols, uint8_t *out)
{
    const uint8_t *r = rec_of(step);
    int n = 0, left = ncols;
    long a = a0;
    if (!r) return -1;
    for (;;) {
        uint8_t b = body_of(r, (uint16_t)(a & 1023)), i;
        const uint8_t *s = gg_sel_bstream +
            ((uint16_t)gg_sel_bptr[b * 2u] | ((uint16_t)gg_sel_bptr[b * 2u + 1u] << 8));
        if (left > CHUNK) {
            uint8_t len = (uint8_t)(gg_sel_bpre[b * 8u + 5u] + 1u);
            for (i = 0; i < len; ++i) out[n++] = s[i];
            left -= CHUNK;
            a += (long)CHUNK * step;
        } else {
            uint8_t len = gg_sel_bpre[b * 8u + (g_mutate == 2 && left > 1 ? left - 2 : left - 1)];
            for (i = 0; i < len; ++i) out[n++] = s[i];
            out[n++] = 6;
            return n;
        }
    }
}

static long g_checked, g_skipped, g_bad;
static int g_shown;

static void one(int16_t step, long a0, int ncols)
{
    uint8_t want[MAXMV], got[MAXMV];
    int nw = ref_span(a0, step, ncols, want), ng;
    if (nw < 0) { ++g_skipped; return; }     /* outside the family the tables cover */
    ng = sel_span(step, a0, ncols, got);
    ++g_checked;
    if (ng != nw || memcmp(want, got, (size_t)nw)) {
        ++g_bad;
        if (g_shown < 8) {
            int k; ++g_shown;
            printf("  MISMATCH step %d acc %ld phase %ld cols %d: want %d got %d\n",
                   step, a0, a0 & 1023, ncols, nw, ng);
            printf("    want:"); for (k = 0; k < nw; ++k) printf(" %d", want[k]); printf("\n");
            printf("    got :"); for (k = 0; k < ng; ++k) printf(" %d", got[k]); printf("\n");
        }
    }
}

static const char *MUTNAME[4] = {"none",
    "threshold compare loses the equal case",
    "terminal chunk uses the previous column's prefix",
    "map entry's bank number loses its top bit"};

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--mutations")) {
        int m, failed = 0;
        printf("mutation control: every injected defect must be caught\n");
        for (m = 1; m <= 3; ++m) {
            int si2, ph2, li2;
            static const int L2[] = {1, 6, 7, 13, 18};
            g_mutate = m; g_checked = 0; g_bad = 0; g_shown = 8;
            for (si2 = 0; si2 < SELFULL_NSTEP; si2 += 7)
                for (ph2 = 0; ph2 < 1024; ph2 += 3)
                    for (li2 = 0; li2 < 5; ++li2)
                        one(k_selfull_step[si2], 65536L + ph2, L2[li2]);
            printf("  %-52s caught %ld of %ld\n", MUTNAME[m], g_bad, g_checked);
            if (!g_bad) { printf("  NOT CAUGHT -- the sweep is blind to this defect\n"); failed = 1; }
        }
        if (failed) { printf("MUTATION_CONTROL_FAIL\n"); return 1; }
        printf("MUTATION_CONTROL_OK all three defects caught\n");
        return 0;
    }
    (void)argv;
{
    /* every terminal-chunk residue, single and multi chunk, and a long run */
    static const int LEN[] = {1, 2, 3, 5, 6, 7, 11, 12, 13, 18, 23, 24};
    static const int NLEN = (int)(sizeof LEN / sizeof LEN[0]);
    /* the accumulator offsets that test translation invariance; every one is a
     * whole number of 1024s, so the phase is unchanged and the body must be too */
    static const long OFF[] = {0L, 1024L * 8, 1024L * 40, 1024L * 63};
    int si, li, oi, ph;

    printf("exhaustive selector validation\n");
    printf("  %d steps x 1024 phases x %d span lengths, reference is the closed-form\n",
           SELFULL_NSTEP, NLEN);
    printf("  column rule in wide integers, not the race kernels\n\n");

    printf("1  every step, every phase, every tested span length\n");
    for (si = 0; si < SELFULL_NSTEP; ++si)
        for (ph = 0; ph < 1024; ++ph)
            for (li = 0; li < NLEN; ++li)
                one(k_selfull_step[si], 65536L + ph, LEN[li]);
    printf("   checked %ld, skipped %ld outside the family, mismatches %ld\n\n",
           g_checked, g_skipped, g_bad);

    { long c0 = g_checked, b0 = g_bad;
      printf("2  translation invariance: the same phase at four accumulator offsets\n");
      for (si = 0; si < SELFULL_NSTEP; ++si)
          for (ph = 0; ph < 1024; ph += 7)
              for (oi = 0; oi < 4; ++oi) {
                  one(k_selfull_step[si], OFF[oi] + ph, 6);
                  one(k_selfull_step[si], OFF[oi] + ph, 13);
              }
      printf("   checked %ld, mismatches %ld\n\n", g_checked - c0, g_bad - b0); }

    { long c0 = g_checked, b0 = g_bad;
      static const int EXC[] = {0, 256, -256, 512, -512, 768, -768,
                                1024, -1024, 1280, -1280, 1536, -1536};
      int i;
      printf("3  the thirteen exceptional multiples of 256, every phase, lengths 1..24\n");
      for (i = 0; i < 13; ++i)
          for (ph = 0; ph < 1024; ++ph)
              for (li = 1; li <= 24; ++li)
                  one((int16_t)EXC[i], 65536L + ph, li);
      printf("   checked %ld, mismatches %ld\n\n", g_checked - c0, g_bad - b0); }

    if (g_bad) { printf("SELECTOR_EXHAUSTIVE_FAIL %ld mismatches\n", g_bad); return 1; }
    printf("SELECTOR_EXHAUSTIVE_OK %ld span evaluations, zero mismatches\n", g_checked);
    return 0;
}
}
