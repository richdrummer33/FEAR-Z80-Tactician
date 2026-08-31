#include "tilesector_room_catalog_poc.h"

static uint8_t straight_bundle(uint32_t seed,uint32_t key){
    static const uint8_t k_straight[] = {0u,1u,4u,6u,7u};
    uint32_t x=tsp_stream_mix32(key^seed^UINT32_C(0xA5A5A5A5));
    return k_straight[x%(uint32_t)(sizeof(k_straight)/sizeof(k_straight[0]))];
}

uint8_t tsp_room_catalog_choose(uint32_t seed,const TSPStreamNodeDesc *node,
                                uint8_t exit_index,TSPRoomCatalogChoice *out){
    TSPRoomCatalogChoice c;
    if(!node||!out)return 0u;

    switch((TSPModuleKind)node->module_kind){
        case TSP_MODULE_HALL_STRAIGHT:
            if(exit_index!=0u)return 0u;
            c.bundle_id=straight_bundle(seed,node->key);
            c.entry_portal=0u;c.exit_portal=1u;
            break;

        case TSP_MODULE_TURN_LEFT:
            if(exit_index!=0u)return 0u;
            c.bundle_id=5u;c.entry_portal=0u;c.exit_portal=1u;
            break;

        case TSP_MODULE_TURN_RIGHT:
            if(exit_index!=0u)return 0u;
            c.bundle_id=5u;c.entry_portal=1u;c.exit_portal=0u;
            break;

        case TSP_MODULE_T_SPLIT:
            if(exit_index>=2u)return 0u;
            c.bundle_id=2u;c.entry_portal=0u;c.exit_portal=(uint8_t)(exit_index+1u);
            break;

        case TSP_MODULE_STAIR_QUARTER_UP_RIGHT:
            if(exit_index!=0u)return 0u;
            c.bundle_id=3u;c.entry_portal=0u;c.exit_portal=1u;
            break;

        case TSP_MODULE_STAIR_QUARTER_DOWN_LEFT:
            if(exit_index!=0u)return 0u;
            c.bundle_id=3u;c.entry_portal=1u;c.exit_portal=0u;
            break;

        /* ROOM_SPLIT has three forward exits (four portals including parent),
         * while the current authored split bundle has two. The opposite stair
         * handedness also needs its own mirrored visual module. Do not silently
         * lie about either shape. */
        default:
            return 0u;
    }

    *out=c;
    return 1u;
}
