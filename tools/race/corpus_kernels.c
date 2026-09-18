/* Replay the renderer's real raster tuples through both kernels.
 *
 * This is the projection with the modelling taken out. Earlier numbers weighted
 * per-length timings by a span-length histogram, which corrects for span length
 * but still samples (step, phase) from 21 hand-chosen steps at centred
 * accumulators. Both kernels care about more than length: the linear scan's cost
 * depends on which of the eight phase bands the span starts in, and the replay's
 * on how many move bytes the body expands to.
 *
 * So the cases here are actual (iq, step, columns) tuples the pose corpus
 * produced, sampled uniformly. Summing measured cycles over them estimates the
 * true mean directly.
 *
 * The kernels also have to agree, on every one of the real tuples, or the
 * runner withholds the timings -- the same gate the race uses.
 */
#include <gbdk/platform.h>
#include <stdint.h>
#include "selfull_hdr.h"
#include "corpus_hdr.h"

#define MAXMV 96

volatile uint8_t g_cp_phase;
uint16_t g_cp_done, g_cp_bad, g_cp_i, g_cp_iter;
int16_t  g_cp_badstep;
uint16_t g_cp_badiq;
uint8_t  g_cp_badcols;

int16_t g_as_iq, g_as_step;
uint8_t g_as_n, g_as_ret;
uint8_t *g_as_outp;
void dda_span_asm(void);
void bplf_span_asm(void);
uint8_t g_out_a[MAXMV], g_out_b[MAXMV];
uint8_t g_n_a, g_n_b;

void main(void)
{
    uint16_t i;
    g_cp_done = 0; g_cp_bad = 0; g_cp_iter = 0; g_cp_phase = 0;
    for (;;) {
        for (i = 0; i < CORPUS_N; ++i) {
            int16_t iq, step;
            uint8_t ncols, k, wrong = 0;

            /* the tuple lives in banked ROM; reading it is driver time, not
             * kernel time, so it happens with the phase marker clear */
            g_cp_i = i;
            SWITCH_ROM2(CORPUS_BANK_IQ);   iq = (int16_t)gg_corpus_iq[i];
            SWITCH_ROM2(CORPUS_BANK_STEP); step = (int16_t)gg_corpus_step[i];
            SWITCH_ROM2(CORPUS_BANK_COLS); ncols = gg_corpus_cols[i];

            g_as_iq = iq; g_as_step = step; g_as_n = ncols;

            g_as_outp = g_out_a;
            g_cp_phase = 1; dda_span_asm(); g_cp_phase = 0;
            g_n_a = g_as_ret;

            g_as_outp = g_out_b;
            g_cp_phase = 2; bplf_span_asm(); g_cp_phase = 0;
            g_n_b = g_as_ret;

            if (g_n_a != g_n_b) wrong = 1;
            else for (k = 0; k < g_n_a; ++k) if (g_out_a[k] != g_out_b[k]) wrong = 1;
            if (wrong) {
                if (!g_cp_bad) { g_cp_badiq = (uint16_t)iq; g_cp_badstep = step; g_cp_badcols = ncols; }
                ++g_cp_bad;
            }
        }
        ++g_cp_iter;
        g_cp_done = 0xC0B5;
    }
}
