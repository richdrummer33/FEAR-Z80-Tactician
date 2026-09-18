/* On-device stress sweep for the production selector.
 *
 * The host sweep validates the tables and the algorithm. It cannot validate the
 * things that only exist on the cartridge: five record banks reached through a
 * real Frame-2 mapper write, a three-byte map entry that carries a bank number,
 * and the hand-written Z80 scan rather than a C transcription of it. Those are
 * exactly where a silent failure would live, so they get their own sweep.
 *
 * The comparison is against the hand-written DDA, lifted verbatim from the race.
 * Two independent implementations must agree on every case; the DDA itself was
 * validated exhaustively on the host against the closed-form column rule, so the
 * chain is complete.
 *
 * Phases are chosen adversarially rather than randomly. A scan over sorted
 * thresholds fails at a boundary or not at all, so every one of the eight band
 * edges is tested at the edge, one below and one above. Random phases inside a
 * band would mostly re-test the easy case.
 */
#include <gbdk/platform.h>
#include <stdint.h>
#include "selfull_hdr.h"

#if SELFULL_BANK0 != 2 || SELFULL_MAPBANK != 7
#error "stress_asm.s hardcodes SEL_BANK0=2 and SEL_MAPBANK=7; regenerate or fix both"
#endif

volatile uint8_t g_st_phase;
uint16_t g_st_done, g_st_bad, g_st_skip;
uint16_t g_st_cases_lo, g_st_cases_hi;
uint16_t g_st_step_i;
int16_t  g_st_badstep;
uint16_t g_st_badphase;
uint8_t  g_st_badlen, g_st_badnw, g_st_badng;

int16_t g_as_iq, g_as_step;
uint8_t g_as_n, g_as_ret;
uint8_t *g_as_outp;
void dda_span_asm(void);
void bplf_span_asm(void);
uint8_t g_out_a[64], g_out_b[64];

#define NLEN 5
static const uint8_t k_len[NLEN] = {1, 6, 7, 13, 18};

/* An accumulator congruent to this phase that keeps the whole span inside the
 * renderer's eight-bit depth domain. Outside it the int16 DDA is not defined,
 * so a case that cannot be placed is skipped and counted rather than faked. */
static uint16_t pick_acc(uint16_t ph, int16_t step, uint8_t ncols)
{
    uint16_t mag = (uint16_t)(step < 0 ? -step : step);
    uint16_t ext = (uint16_t)(mag * (uint16_t)ncols);
    uint16_t lo, hi, a;
    if (ext > 16383u) return 0xFFFFu;
    if (step >= 0) { lo = 0u; hi = (uint16_t)(16383u - ext); }
    else           { lo = ext; hi = 16383u; }
    a = ph;
    while (a < lo) a = (uint16_t)(a + 1024u);
    if (a > hi) return 0xFFFFu;
    return a;
}

static void bump(void)
{
    if (++g_st_cases_lo == 0u) ++g_st_cases_hi;
}

void main(void)
{
    uint16_t si;
    g_st_done = 0; g_st_bad = 0; g_st_skip = 0;
    g_st_cases_lo = 0; g_st_cases_hi = 0; g_st_phase = 1;

    for (si = 0; si < SELFULL_NSTEP; ++si) {
        int16_t step;
        uint16_t edge[8], acc = 0;
        uint8_t c, d, li;

        g_st_step_i = si;
        SWITCH_ROM2(SELFULL_STEPBANK);
        step = (int16_t)((uint16_t)gg_selfull_steps[si * 2u] |
                         ((uint16_t)gg_selfull_steps[si * 2u + 1u] << 8));
        for (c = 0; c < 8u; ++c) { edge[c] = (uint16_t)(acc & 1023u); acc = (uint16_t)(acc - (uint16_t)step); }

        for (c = 0; c < 8u; ++c) for (d = 0; d < 3u; ++d) {
            uint16_t ph = (uint16_t)((edge[c] + 1023u + d) & 1023u);
            for (li = 0; li < NLEN; ++li) {
                uint8_t ncols = k_len[li], k;
                uint16_t a0 = pick_acc(ph, step, ncols);
                uint8_t nw, ng, wrong = 0;
                if (a0 == 0xFFFFu) { ++g_st_skip; continue; }

                g_as_iq = (int16_t)(a0 - 32u);
                g_as_step = step; g_as_n = ncols;
                g_as_outp = g_out_a; dda_span_asm(); nw = g_as_ret;
                g_as_outp = g_out_b; bplf_span_asm(); ng = g_as_ret;

                if (nw != ng) wrong = 1;
                else for (k = 0; k < nw; ++k) if (g_out_a[k] != g_out_b[k]) wrong = 1;
                if (wrong) {
                    if (!g_st_bad) {
                        g_st_badstep = step; g_st_badphase = ph;
                        g_st_badlen = ncols; g_st_badnw = nw; g_st_badng = ng;
                    }
                    ++g_st_bad;
                }
                bump();
            }
        }
    }
    g_st_phase = 0;
    g_st_done = 0xD01E;
    for (;;) { }
}
