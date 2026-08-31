#ifndef TILESECTOR_ROOM_CATALOG_POC_H
#define TILESECTOR_ROOM_CATALOG_POC_H

#include <stdint.h>
#include "tilesector_world_stream_poc.h"

typedef struct TSPRoomCatalogChoice {
    uint8_t bundle_id;
    uint8_t entry_portal;
    uint8_t exit_portal;
} TSPRoomCatalogChoice;

/* Resolve one procedural topology node + chosen topology exit into a reusable
 * authored visual bundle/route. Returns zero when the current PoC catalog has
 * no faithful authored representation for that topology yet. */
uint8_t tsp_room_catalog_choose(uint32_t seed,const TSPStreamNodeDesc *node,
                                uint8_t exit_index,TSPRoomCatalogChoice *out);

#endif
