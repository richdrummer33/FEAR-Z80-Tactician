#pragma once
#include <stdint.h>

/* Shared Game Gear runtime for the finite run-edge program target.
 *
 * This is deliberately renderer-agnostic: it selects one compiled run-edge
 * body from the sparse direct pack and replays its (word, signed-destination-
 * delta) stream into a caller-supplied guarded name-table buffer. The standalone
 * ROM semantic probe and the playable renderer must use this same code so the
 * bank-switching/dispatch implementation cannot drift between validation and
 * integration.
 */

typedef struct TSPProgjoinBodyRef {
    uint8_t bank;
    uint16_t off;
} TSPProgjoinBodyRef;

/* Returns 1 when the exact semantic key exists in the packed vocabulary. */
uint8_t tsp_progjoin_dispatch_body(int16_t step, uint8_t fam, uint8_t want,
                                   int16_t iq, TSPProgjoinBodyRef *out);

/* Replays one selected body. `cursor` is a byte offset in dst and is updated to
 * the exact target continuation position. Returns 0 rather than writing outside
 * dst. The target's normal +7/-7 row guard should make that path unreachable. */
uint8_t tsp_progjoin_play_body(TSPProgjoinBodyRef body,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor);
