#include "tilesector_world_module.h"

/* No multiply/table required: world expansion happens only at connector
 * crossings, but keep the selector cheap enough that even repeated backtrack
 * regeneration is negligible. */
static uint16_t mix16(uint16_t x){
    x^=(uint16_t)(x<<7);
    x^=(uint16_t)(x>>9);
    x^=(uint16_t)(x<<8);
    return x;
}

static const TSPModuleTopology k_topology[TSP_MODULE_KIND_COUNT] = {
    /* straight */ {1u, {{ 0,0},{0,0},{0,0}}},
    /* left     */ {1u, {{-1,0},{0,0},{0,0}}},
    /* right    */ {1u, {{ 1,0},{0,0},{0,0}}},
    /* room     */ {3u, {{ 0,0},{-1,0},{1,0}}},
    /* T        */ {2u, {{-1,0},{ 1,0},{0,0}}},
    /* up left  */ {1u, {{-1,TSP_MODULE_STAIR_RISE_Q4},{0,0},{0,0}}},
    /* up right */ {1u, {{ 1,TSP_MODULE_STAIR_RISE_Q4},{0,0},{0,0}}},
    /* dn left  */ {1u, {{-1,-TSP_MODULE_STAIR_RISE_Q4},{0,0},{0,0}}},
    /* dn right */ {1u, {{ 1,-TSP_MODULE_STAIR_RISE_Q4},{0,0},{0,0}}}
};

const TSPModuleTopology *tsp_module_topology(TSPModuleKind kind){
    if((uint8_t)kind>=TSP_MODULE_KIND_COUNT)return (const TSPModuleTopology *)0;
    return &k_topology[(uint8_t)kind];
}

TSPModuleKind tsp_module_choose(uint16_t seed,uint16_t node_key,uint8_t allow_vertical){
    uint16_t x=(uint16_t)(seed^(uint16_t)(node_key+0x6d2bu));
    uint8_t r;
    x=mix16(x);
    x=mix16((uint16_t)(x^(node_key<<3)^(node_key>>5)));
    r=(uint8_t)(x&15u);

    /* Weighted but intentionally simple prototype grammar:
     * 5/16 straight, 2/16 each ordinary turn, 3/16 split/room,
     * 4/16 vertical quarter-stair. Later the caller can apply biome/mission
     * weights without changing connector semantics. */
    if(r<5u)return TSP_MODULE_HALL_STRAIGHT;
    if(r<7u)return TSP_MODULE_TURN_LEFT;
    if(r<9u)return TSP_MODULE_TURN_RIGHT;
    if(r<11u)return TSP_MODULE_ROOM_SPLIT;
    if(r==11u)return TSP_MODULE_T_SPLIT;
    if(!allow_vertical)return (r&1u)?TSP_MODULE_TURN_LEFT:TSP_MODULE_TURN_RIGHT;
    if(r==12u)return TSP_MODULE_STAIR_QUARTER_UP_LEFT;
    if(r==13u)return TSP_MODULE_STAIR_QUARTER_UP_RIGHT;
    if(r==14u)return TSP_MODULE_STAIR_QUARTER_DOWN_LEFT;
    return TSP_MODULE_STAIR_QUARTER_DOWN_RIGHT;
}

uint8_t tsp_module_exit(TSPModuleKind kind,uint8_t incoming_dir,uint8_t exit_index,
                        uint8_t *out_dir,int16_t *out_dz_q4){
    const TSPModuleTopology *t=tsp_module_topology(kind);
    int8_t turn;
    if(!t||exit_index>=t->exit_count)return 0u;
    turn=t->exits[exit_index].turn;
    if(out_dir)*out_dir=(uint8_t)((incoming_dir+(uint8_t)(turn+4))&3u);
    if(out_dz_q4)*out_dz_q4=t->exits[exit_index].dz_q4;
    return 1u;
}
