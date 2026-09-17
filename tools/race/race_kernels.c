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

void main(void)
{
    uint8_t li, ci;
    g_race_phase = 0;
    g_race_iter = 0; g_race_mismatch = 0; g_race_cases = 0;
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
                uint8_t ph = (uint8_t)(li * 4u + 1u);

                g_race_phase = ph;
                g_n_dda = dda_span(iq, st, ncols, g_out_dda);

                g_race_phase = (uint8_t)(ph + 1u);
                band_setup(st);
                g_race_phase = (uint8_t)(ph + 2u);
                g_n_band = band_span(iq, st, ncols, g_out_band);

                g_race_phase = (uint8_t)(ph + 3u);
                g_n_pack = pack_span(&k_race_blob[k_race_off[idx]], k_race_mlen[idx], g_out_pack);

                g_race_phase = 0;
                if (g_n_dda != k_race_mlen[idx]) bad = 1;
                if (g_n_band != k_race_mlen[idx]) bad = 1;
                if (g_n_pack != k_race_mlen[idx]) bad = 1;
                for (k = 0; k < k_race_mlen[idx]; ++k) {
                    uint8_t want = k_race_blob[k_race_off[idx] + k];
                    if (g_out_dda[k] != want || g_out_band[k] != want || g_out_pack[k] != want) bad = 1;
                }
                if (bad) ++g_race_mismatch;
                ++g_race_cases;
            }
        }
        ++g_race_iter;
    }
}
