#pragma once
#include <stdint.h>

/* Shared Game Gear runtime for the finite run-edge program target.
 *
 * This is deliberately renderer-agnostic: it selects compiled run-edge bodies
 * from the sparse direct pack and replays their (word, signed-destination-
 * delta) streams into a caller-supplied guarded name-table buffer. The
 * standalone ROM semantic probe and the playable renderer must use this same
 * code so the bank-switching/dispatch implementation cannot drift between
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

/* Replays one selected body. `cursor` is a byte offset in dst and is updated to
 * the exact target continuation position. Returns 0 rather than writing outside
 * dst. The target's normal +7/-7 row guard should make that path unreachable. */
uint8_t tsp_progjoin_play_body(TSPProgjoinBodyRef body,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);

/* Replay a previously preflighted chain. Selection cannot fail here; only a
 * destination-bounds failure can stop playback. */
uint8_t tsp_progjoin_play_plan(const TSPProgjoinRunPlan *plan,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);
