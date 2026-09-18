/* Z80 race: three implementations of ONE function, on real hardware timing.
 *
 * The function: given (iq, step) and a span length supplied from outside, emit
 * the raster move stream. Span length is external for the DDA and the band
 * transducer, so chunk chaining is charged rather than hidden by benchmarking
 * only the six-column case.
 *
 *   phase 1  DDA        straight-line arithmetic, no table at all
 *   phase 2  BAND setup once per run: the eight edge phases p = (-c*step) mod 1024
 *   phase 3  BAND chunk classify the band, replay its six-byte program, memoising
 *                       each band's program the first time it is needed
 *   phase 4  PACKED     copy a stored stream, charged NOTHING for identifying
 *                       which stream it needs
 *
 * PACKED is deliberately flattered. It is a floor on what any table-driven
 * approach could cost, not a proposal: a real one would have to pay for
 * identification, and identification is exactly where the sampled dictionary
 * died. If PACKED wins by a little, the table is not worth it; if it wins by a
 * lot, the question becomes whether the generic machine can be arranged to fetch
 * as cheaply.
 *
 * All three write to separate buffers and the driver compares them against each
 * other AND against the generated expectation, so a fast kernel that is wrong
 * cannot win.
 *
 * These are SDCC C, like the renderer's own C paths. Hand assembly would improve
 * all three and is a separate question; what is being asked here is which SHAPE
 * of algorithm the Z80 prefers.
 */
#include <gbdk/platform.h>
#include <stdint.h>
#include <string.h>

#include "race_kernels.h"

/* sampled per instruction by the profiler to attribute master cycles */
volatile uint8_t g_race_phase;
uint8_t g_out_dda[MAXMV], g_out_band[MAXMV], g_out_pack[MAXMV];
uint8_t g_n_dda, g_n_band, g_n_pack;
uint16_t g_race_iter;
uint16_t g_race_mismatch;
uint16_t g_race_cases;
/* one bit per kernel, so a failure says WHICH kernel rather than just how many */
uint16_t g_race_kmask;
uint8_t g_race_first[8];

/* argument block for the hand-written kernels: globals instead of the stack,
 * so the calling convention costs nothing either side of the comparison */
int16_t g_as_iq, g_as_step;
uint8_t g_as_n, g_as_mlen, g_as_ret, g_as_ord;
uint8_t *g_as_outp;
const uint8_t *g_as_srcp;
void dda_span_asm(void);
void pack_span_asm(void);
void a0_span_asm(void);
void a1_span_asm(void);
void bfl_span_asm(void);
void bfb_span_asm(void);
void bpl_span_asm(void);
uint8_t g_out_dasm[MAXMV], g_out_pasm[MAXMV];
uint8_t g_n_dasm, g_n_pasm;
uint8_t g_out_sel[5][MAXMV];
uint8_t g_n_sel[5];

void main(void)
{
    uint8_t li, ci;
    g_race_phase = 0;
    g_race_iter = 0; g_race_mismatch = 0; g_race_cases = 0; g_race_kmask = 0;
    for (;;) {
        for (li = 0; li < RACE_NLEN; ++li) {
            uint8_t ncols = k_race_len[li];
            for (ci = 0; ci < RACE_NCASE; ++ci) {
                int16_t iq = k_race_iq[ci], st = k_race_step[ci];
                uint16_t idx = (uint16_t)li * RACE_NCASE + ci;
                uint8_t k, bad = 0;

                /* phase = li*4 + kernel + 1, so cycles are attributed per span
                 * length as well as per kernel. Aggregating over lengths hides
                 * the only question chunk chaining can answer. */
                uint8_t ph = (uint8_t)(li * RACE_NK + 1u);

                g_race_phase = ph;
                g_n_dda = dda_span(iq, st, ncols, g_out_dda);

                g_race_phase = (uint8_t)(ph + 1u);
                band_setup(st);
                g_race_phase = (uint8_t)(ph + 2u);
                g_n_band = band_span(iq, st, ncols, g_out_band);

                g_race_phase = (uint8_t)(ph + 3u);
                g_n_pack = pack_span(&k_race_blob[k_race_off[idx]], k_race_mlen[idx], g_out_pack);

                g_as_iq = iq; g_as_step = st; g_as_n = ncols; g_as_outp = g_out_dasm;
                g_race_phase = (uint8_t)(ph + 4u);
                dda_span_asm();
                g_n_dasm = g_as_ret;

                g_as_srcp = &k_race_blob[k_race_off[idx]];
                g_as_mlen = k_race_mlen[idx]; g_as_outp = g_out_pasm;
                g_race_phase = (uint8_t)(ph + 5u);
                pack_span_asm();
                g_n_pasm = g_as_ret;

                /* The selector ladder. Each one is charged its whole path:
                 * bank selection, address formation, the search, the body fetch
                 * and the replay, with the span length supplied from outside so
                 * chunk chaining is paid for rather than benchmarked away. */
                g_as_ord = ci;              /* A0 is handed the ordinal free */
                g_as_outp = g_out_sel[0];
                g_race_phase = (uint8_t)(ph + 6u);
                a0_span_asm();
                g_n_sel[0] = g_as_ret;

                g_as_ord = 0xFF;            /* A1 has to derive it from the step */
                g_as_outp = g_out_sel[1];
                g_race_phase = (uint8_t)(ph + 7u);
                a1_span_asm();
                g_n_sel[1] = g_as_ret;
                if (g_as_ord != ci) bad = 1;

                g_as_ord = ci;
                g_as_outp = g_out_sel[2];
                g_race_phase = (uint8_t)(ph + 8u);
                bfl_span_asm();
                g_n_sel[2] = g_as_ret;

                g_as_outp = g_out_sel[3];
                g_race_phase = (uint8_t)(ph + 9u);
                bfb_span_asm();
                g_n_sel[3] = g_as_ret;

                g_as_outp = g_out_sel[4];
                g_race_phase = (uint8_t)(ph + 10u);
                bpl_span_asm();
                g_n_sel[4] = g_as_ret;

                g_race_phase = 0;
                if (g_n_dda != k_race_mlen[idx]) bad = 1;
                if (g_n_band != k_race_mlen[idx]) bad = 1;
                if (g_n_pack != k_race_mlen[idx]) bad = 1;
                if (g_n_dasm != k_race_mlen[idx]) bad = 1;
                if (g_n_pasm != k_race_mlen[idx]) bad = 1;
                for (k = 0; k < 5u; ++k) {
                    uint8_t j, w = 0, ml = k_race_mlen[idx];
                    if (g_n_sel[k] != ml) w = 1;
                    else for (j = 0; j < ml; ++j)
                        if (g_out_sel[k][j] != k_race_blob[k_race_off[idx] + j]) w = 1;
                    if (w) { bad = 1;
                        if (!(g_race_kmask & (1u << k))) {
                            g_race_kmask |= (uint16_t)(1u << k);
                            if (k == 0) { g_race_first[0] = ml; g_race_first[1] = g_n_sel[0];
                                          g_race_first[2] = g_out_sel[0][0];
                                          g_race_first[3] = k_race_blob[k_race_off[idx]]; } } }
                }
                for (k = 0; k < k_race_mlen[idx]; ++k) {
                    uint8_t want = k_race_blob[k_race_off[idx] + k];
                    if (g_out_dda[k] != want) { bad = 1; g_race_kmask |= 0x100u; }
                    if (g_out_band[k] != want) { bad = 1; g_race_kmask |= 0x200u; }
                    if (g_out_pack[k] != want) { bad = 1; g_race_kmask |= 0x400u; }
                    if (g_out_dasm[k] != want) { bad = 1; g_race_kmask |= 0x800u; }
                    if (g_out_pasm[k] != want) { bad = 1; g_race_kmask |= 0x1000u; }
                }
                if (bad) ++g_race_mismatch;
                ++g_race_cases;
            }
        }
        ++g_race_iter;
    }
}
