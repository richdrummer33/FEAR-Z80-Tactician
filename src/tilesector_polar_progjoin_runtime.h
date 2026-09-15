#pragma once
#include <stdint.h>

/* Shared Game Gear runtime for the finite run-edge program target.
 *
 * This is deliberately renderer-agnostic: it selects compiled run-edge bodies
 * from the sparse direct pack and replays their (word, signed-destination-
 * delta) streams. The standalone ROM semantic probe and the playable renderer
 * must use this same code so bank switching and dispatch cannot drift between
 * validation and integration.
 */

#define TSP_PROGJOIN_CHUNK_COLS 6u
#define TSP_PROGJOIN_MAX_RUN_COLS 20u
#define TSP_PROGJOIN_MAX_CHUNKS 4u

typedef struct TSPProgjoinBodyRef {
    uint8_t bank;
    uint16_t off;
} TSPProgjoinBodyRef;

typedef struct TSPProgjoinRunPlan {
    uint8_t count;
    uint8_t wants[TSP_PROGJOIN_MAX_CHUNKS];
    TSPProgjoinBodyRef bodies[TSP_PROGJOIN_MAX_CHUNKS];
} TSPProgjoinRunPlan;

uint8_t tsp_progjoin_dispatch_body(int16_t step, uint8_t fam, uint8_t want,
                                   int16_t iq, TSPProgjoinBodyRef *out);
uint8_t tsp_progjoin_preflight_run(int16_t step, uint8_t fam, uint8_t ncol,
                                   int16_t iq0, TSPProgjoinRunPlan *out);
uint8_t tsp_progjoin_play_body(TSPProgjoinBodyRef body,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);
uint8_t tsp_progjoin_play_plan(const TSPProgjoinRunPlan *plan,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);

/* Live Game Gear playback against the existing 3-byte-per-column ownership
 * mask. The five observed destination motions carry row/column/coverage state
 * without adding metadata to the compiled body. */
uint8_t tsp_progjoin_play_plan_gated(const TSPProgjoinRunPlan *plan,
                                     uint16_t *dst_words,
                                     const uint8_t *coverage60,
                                     uint8_t *row_min18,
                                     uint8_t *row_max18,
                                     int8_t first_row,
                                     uint8_t first_col);

#if defined(TSPF_PROGJOIN_FULL) && TSPF_PROGJOIN_FULL
/* Playable integration bridge. Kept in the PROGJOIN runtime translation unit
 * so the already-full renderer bank does not absorb selector/setup code. It
 * preflights both FULL edge families before any writes and returns 1 only when
 * both compiled edges were emitted against pre-surface ownership. */
uint8_t tsp_progjoin_try_full_edges(uint16_t *out, uint8_t c0, uint8_t n,
                                    int16_t iq, int16_t step);
#endif
