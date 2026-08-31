#include <stdint.h>
#include <stdio.h>
#include "tilesector_polar.h"
#include "tilesector_room_poc.h"

#define Q(v) ((int16_t)((v)*16))

int main(void){
    uint8_t id,p;
    TSPState s;

    for(id=0u;id<TSP_ROOM_POC_COUNT;++id){
        const TSPRoomPocDef *r=tsp_room_poc_get(id);
        if(!r){fprintf(stderr,"missing room %u\n",id);return 10;}
        if(r->asset_id!=id){fprintf(stderr,"room id drift %u/%u\n",id,r->asset_id);return 11;}
        if(!r->walk_count||r->walk_count>TSP_ROOM_POC_MAX_WALK_RECTS)return 12;
        if(r->portal_count<2u||r->portal_count>TSP_ROOM_POC_MAX_PORTALS)return 13;
        if(!tsp_room_poc_is_walkable(r,r->spawn_x_q4,r->spawn_y_q4)){
            fprintf(stderr,"spawn not walkable room=%u\n",id);return 14;
        }
        for(p=0u;p<r->portal_count;++p){
            if(!tsp_room_poc_is_walkable(r,r->portal[p].x_q4,r->portal[p].y_q4)){
                fprintf(stderr,"portal %u not walkable room=%u\n",p,id);return 15;
            }
        }
    }

    if(tsp_room_poc_floor_z(tsp_room_poc_get(4u),Q(16),Q(36))!=Q(0))return 20;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(4u),Q(40),Q(36))!=Q(2))return 21;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(4u),Q(72),Q(36))!=Q(4))return 22;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(5u),Q(48),Q(36))!=Q(-2))return 23;

    if(tsp_room_poc_floor_z(tsp_room_poc_get(6u),Q(16),Q(32))!=Q(0))return 24;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(6u),Q(30),Q(32))!=Q(1))return 25;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(6u),Q(52),Q(32))!=Q(2))return 26;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(6u),Q(52),Q(52))!=Q(3))return 27;
    if(tsp_room_poc_floor_z(tsp_room_poc_get(6u),Q(52),Q(68))!=Q(4))return 28;

    if(tsp_room_poc_is_walkable(tsp_room_poc_get(8u),Q(48),Q(40))){
        fprintf(stderr,"pillar blocker is walkable\n");return 29;
    }

    if(!tsp_room_poc_set_active(4u))return 30;
    tsp_reset(&s);
    if(s.x_q4!=tsp_room_poc_get(4u)->spawn_x_q4 ||
       s.y_q4!=tsp_room_poc_get(4u)->spawn_y_q4 ||
       s.z_q4!=(int16_t)(TSP_EYE_HEIGHT_Q4+Q(0))){
        fprintf(stderr,"room-local reset mismatch x=%d y=%d z=%d\n",s.x_q4,s.y_q4,s.z_q4);
        return 31;
    }
    if(!tsp_is_walkable_q4(Q(40),Q(36)))return 32;
    if(tsp_floor_z_q4(Q(40),Q(36))!=Q(2))return 33;

    {
        int16_t max_z=s.z_q4;
        unsigned i;
        for(i=0u;i<220u;++i){
            tsp_step(&s,TSP_INPUT_UP);
            if(s.z_q4>max_z)max_z=s.z_q4;
        }
        if(max_z<(int16_t)(TSP_EYE_HEIGHT_Q4+Q(4))){
            fprintf(stderr,"movement never crossed raised floor max_z=%d\n",max_z);
            return 34;
        }
        if(!tsp_is_walkable_q4(s.x_q4,s.y_q4))return 35;
    }

    printf("room_local_poc PASS rooms=%u step_z=%d sunken_z=%d stair_top_z=%d\n",
           TSP_ROOM_POC_COUNT,
           tsp_room_poc_floor_z(tsp_room_poc_get(4u),Q(72),Q(36)),
           tsp_room_poc_floor_z(tsp_room_poc_get(5u),Q(48),Q(36)),
           tsp_room_poc_floor_z(tsp_room_poc_get(6u),Q(52),Q(68)));
    return 0;
}
