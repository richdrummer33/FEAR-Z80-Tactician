#include "tilesector_room_poc.h"

#define Q(v) ((int16_t)((v)*16))
#define R(a,b,c,d) {Q(a),Q(b),Q(c),Q(d)}
#define F(a,b,c,d,z) {R(a,b,c,d),Q(z)}
#define P(x,y,d,i) {Q(x),Q(y),(uint8_t)(d),(uint8_t)(i)}
#define ER R(0,0,0,0)
#define EF F(0,0,0,0,0)
#define EP P(0,0,0,0)

static const TSPRoomPocDef k_rooms[TSP_ROOM_POC_COUNT] = {
    /* 0: long straight hall, unlit candidate. */
    {0u,TSP_MODULE_HALL_STRAIGHT,1u,0u,0u,2u,Q(0),Q(16),Q(32),
     {R(8,20,88,44),ER,ER,ER},{ER,ER},{EF,EF,EF,EF,EF},
     {P(8,32,2,0),P(88,32,0,1),EP}},

    /* 1: dog-leg chamber. */
    {1u,TSP_MODULE_TURN_RIGHT,2u,0u,0u,2u,Q(0),Q(16),Q(32),
     {R(8,20,56,44),R(44,20,68,80),ER,ER},{ER,ER},{EF,EF,EF,EF,EF},
     {P(8,32,2,0),P(56,80,1,1),EP}},

    /* 2: broad irregular three-portal room. */
    {2u,TSP_MODULE_ROOM_SPLIT,2u,0u,0u,3u,Q(0),Q(16),Q(40),
     {R(8,16,88,64),R(28,56,68,80),ER,ER},{ER,ER},{EF,EF,EF,EF,EF},
     {P(8,40,2,0),P(88,40,0,1),P(48,80,1,2)}},

    /* 3: explicit T split; both exit mouths can be visible from the stem. */
    {3u,TSP_MODULE_T_SPLIT,2u,0u,0u,3u,Q(0),Q(48),Q(76),
     {R(12,20,84,48),R(36,40,60,88),ER,ER},{ER,ER},{EF,EF,EF,EF,EF},
     {P(48,88,1,0),P(12,34,2,1),P(84,34,0,2)}},

    /* 4: step-up room: floor 0 -> +2 -> +4. */
    {4u,TSP_MODULE_HALL_STRAIGHT,1u,0u,2u,2u,Q(0),Q(16),Q(36),
     {R(8,20,88,52),ER,ER,ER},{ER,ER},
     {F(32,20,55,52,2),F(56,20,88,52,4),EF,EF,EF},
     {P(8,36,2,0),P(88,36,0,1),EP}},

    /* 5: sunken room: central section drops two world units. */
    {5u,TSP_MODULE_HALL_STRAIGHT,1u,0u,1u,2u,Q(0),Q(16),Q(36),
     {R(8,20,88,52),ER,ER,ER},{ER,ER},
     {F(32,20,64,52,-2),EF,EF,EF,EF},
     {P(8,36,2,0),P(88,36,0,1),EP}},

    /* 6: right-handed quarter stair, four one-unit height bands. */
    {6u,TSP_MODULE_STAIR_QUARTER_UP_RIGHT,2u,0u,4u,2u,Q(0),Q(16),Q(32),
     {R(8,20,64,44),R(40,20,64,76),ER,ER},{ER,ER},
     {F(24,20,39,44,1),F(40,20,64,43,2),F(40,44,64,59,3),F(40,60,64,76,4),EF},
     {P(8,32,2,0),P(52,76,1,1),EP}},

    /* 7: left-handed quarter stair, mirrored topology. */
    {7u,TSP_MODULE_STAIR_QUARTER_UP_LEFT,2u,0u,4u,2u,Q(0),Q(72),Q(32),
     {R(24,20,80,44),R(24,20,48,76),ER,ER},{ER,ER},
     {F(56,20,79,44,1),F(24,20,55,43,2),F(24,44,48,59,3),F(24,60,48,76,4),EF},
     {P(80,32,0,0),P(36,76,1,1),EP}},

    /* 8: broad room with a central opaque/collision pillar. */
    {8u,TSP_MODULE_HALL_STRAIGHT,1u,1u,0u,2u,Q(0),Q(16),Q(40),
     {R(8,16,88,64),ER,ER,ER},{R(40,28,56,52),ER},{EF,EF,EF,EF,EF},
     {P(8,40,2,0),P(88,40,0,1),EP}},

    /* 9: narrow switchback assembled from three local walk rectangles. */
    {9u,TSP_MODULE_TURN_LEFT,3u,0u,0u,2u,Q(0),Q(16),Q(30),
     {R(8,20,48,40),R(36,20,56,68),R(44,48,88,68),ER},
     {ER,ER},{EF,EF,EF,EF,EF},
     {P(8,30,2,0),P(88,58,0,1),EP}}
};

static uint8_t g_active_asset=0xffu;

static uint8_t inside(const TSPRoomPocRect *r,int16_t x,int16_t y){
    return (uint8_t)(x>=r->x0_q4&&x<=r->x1_q4&&y>=r->y0_q4&&y<=r->y1_q4);
}

const TSPRoomPocDef *tsp_room_poc_get(uint8_t asset_id){
    if(asset_id>=TSP_ROOM_POC_COUNT)return (const TSPRoomPocDef *)0;
    return &k_rooms[asset_id];
}

uint8_t tsp_room_poc_set_active(uint8_t asset_id){
    if(asset_id>=TSP_ROOM_POC_COUNT){g_active_asset=0xffu;return 0u;}
    g_active_asset=asset_id;return 1u;
}

const TSPRoomPocDef *tsp_room_poc_active(void){
    return tsp_room_poc_get(g_active_asset);
}

uint8_t tsp_room_poc_is_walkable(const TSPRoomPocDef *room,int16_t x_q4,int16_t y_q4){
    uint8_t i,hit=0u;
    if(!room)return 0u;
    for(i=0u;i<room->walk_count;++i)if(inside(&room->walk[i],x_q4,y_q4)){hit=1u;break;}
    if(!hit)return 0u;
    for(i=0u;i<room->block_count;++i)if(inside(&room->block[i],x_q4,y_q4))return 0u;
    return 1u;
}

int16_t tsp_room_poc_floor_z(const TSPRoomPocDef *room,int16_t x_q4,int16_t y_q4){
    uint8_t i;
    if(!room)return 0;
    for(i=0u;i<room->floor_zone_count;++i)
        if(inside(&room->floor_zone[i].rect,x_q4,y_q4))return room->floor_zone[i].z_q4;
    return room->default_floor_z_q4;
}
