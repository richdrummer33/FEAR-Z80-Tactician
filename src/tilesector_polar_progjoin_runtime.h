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

/* Returns 1 when the exact semantic key exists in the packed vocabulary. */
uint8_t tsp_progjoin_dispatch_body(int16_t step, uint8_t fam, uint8_t want,
                                   int16_t iq, TSPProgjoinBodyRef *out);

/* Resolve every chunk of one run-edge before any playback occurs. This is the
 * atomic-fallback contract for the playable renderer: on 0, no destination
 * state has been modified and the legacy edge path may run unchanged. */
uint8_t tsp_progjoin_preflight_run(int16_t step, uint8_t fam, uint8_t ncol,
                                   int16_t iq0, TSPProgjoinRunPlan *out);

/* Raw semantic playback used by the standalone ROM oracle. `cursor` is a byte
 * offset in dst and is updated to the target continuation position. */
uint8_t tsp_progjoin_play_body(TSPProgjoinBodyRef body,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);
uint8_t tsp_progjoin_play_plan(const TSPProgjoinRunPlan *plan,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);

/* Live Game Gear playback against the existing 3-byte-per-column ownership
 * mask. No ownership metadata is stored in the compiled body. The full corpus
 * has only five destination motions: +40, +2, -38, -78, -118 bytes. Those are
 * sufficient to carry {row,col,row-bit,coverage-index} exactly from the run's
 * known first cell.
 *
 * This routine tests ownership but DOES NOT claim it. That is intentional: top
 * and bottom programs for one FULL wall must both observe the same pre-surface
 * ownership state. The existing materializer claims the wall span afterwards.
 * Dirty row bounds are updated only when a visible word actually changes.
 */
uint8_t tsp_progjoin_play_plan_gated(const TSPProgjoinRunPlan *plan,
                                     uint16_t *dst_words,
                                     const uint8_t *coverage60,
                                     uint8_t *row_min18,
                                     uint8_t *row_max18,
                                     int8_t first_row,
                                     uint8_t first_col);
