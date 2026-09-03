#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "e1m1_room1_world.h"
#include "tilesector_polar.h"
#include "e1m1_room1_polar_pvs.h"
#include "generated/e1m1_room1_exact_floor.h"

extern uint8_t g_tspf_e1_host_all_segments;

static void die(const char *msg){fprintf(stderr,"FAIL: %s\n",msg);}

static void to_polar(const E1Room1State *e,TSPState *p){
    memset(p,0,sizeof(*p));
    p->x_q4=e->x_q4;p->y_q4=e->y_q4;p->z_q4=e->z_q4;p->yaw=e->yaw;
    p->speed_scale=e->speed_scale;
}

static uint16_t wall_cells(const uint16_t m[TSP_MAP_CELLS]){
    uint16_t i,n=0u;
    for(i=0u;i<TSP_MAP_CELLS;++i)
        if((m[i]&TSP_TILE_ID_MASK)>TSP_TILE_HORIZON)++n;
    return n;
}

int main(void){
    E1Room1State e;
    TSPState p;
    uint16_t map[TSP_MAP_CELLS],prev[TSP_MAP_CELLS];
    uint8_t mask[8],i;
    uint16_t map_i,changed=0u,walls;

    e1_room1_reset(&e);
    if(e.x_q4!=(22<<4)||e.y_q4!=(52<<4)){die("wrong Room-1 reset");return 2;}
    if(e.z_q4!=(19<<4)){die("reset must preserve exact start floor + five-unit eye");return 2;}
    if(!e1_room1_is_walkable_q4(e.x_q4,e.y_q4)){die("spawn not walkable");return 2;}

    e1pf_load_pvs(0u,3u,0u,mask);
    for(i=0u;i<8u;++i)changed|=mask[i];
    if(!changed){die("baked PVS returned empty spawn-area mask");return 2;}

    to_polar(&e,&p);
    g_tspf_appearance_mode=0u;
    tsp_polar_renderer_reset();
    tsp_polar_render(&p,map,(TSPColumn *)0);
    walls=wall_cells(map);
    if(!walls){die("mature Polar path emitted no Room-1 wall cells");return 2;}
    if(!g_tspf_active_runs||g_tspf_active_runs>58u){die("bad active-run count");return 2;}
    memcpy(prev,map,sizeof(prev));

    e.yaw=64u;
    to_polar(&e,&p);
    tsp_polar_render(&p,map,(TSPColumn *)0);
    changed=0u;
    for(map_i=0u;map_i<TSP_MAP_CELLS;++map_i)
        if(map[map_i]!=prev[map_i])++changed;
    if(!changed){die("quarter-turn did not change retained name table");return 2;}

    /* Exact stair/floor oracle remains independent of the renderer transplant. */
    if(e1_room1_floor_z_q4(22<<4,52<<4)!=(14<<4)){die("start floor regression");return 2;}
    if(e1_room1_floor_z_q4(62<<4,52<<4)!=(2<<4)){die("stair floor regression");return 2;}
    if(e1_room1_floor_z_q4(70<<4,52<<4)!=0){die("main-room floor regression");return 2;}

    /* Dense host oracle: the compact PVS may reject work, but it must never
     * change the final retained name table relative to admitting every exact
     * Room-1 surface. Sample every integral walkable XY and 32 headings so
     * yaw-bin boundaries and the full authored floor/stair footprint are hit. */
    {
        uint16_t pvs_map[TSP_MAP_CELLS],all_map[TSP_MAP_CELLS];
        uint32_t states=0u;
        int16_t xq,yq;
        uint16_t mi;
        uint8_t yaw;
        for(yq=(int16_t)(E1X_WORLD_MIN_Y<<4);
            yq<(int16_t)(E1X_WORLD_MAX_Y<<4);yq=(int16_t)(yq+16)){
            for(xq=(int16_t)(E1X_WORLD_MIN_X<<4);
                xq<(int16_t)(E1X_WORLD_MAX_X<<4);xq=(int16_t)(xq+16)){
                if(!e1_room1_is_walkable_q4(xq,yq))continue;
                memset(&p,0,sizeof(p));
                p.x_q4=xq;p.y_q4=yq;
                p.z_q4=(int16_t)(E1_EYE_HEIGHT_Q4+e1_room1_floor_z_q4(xq,yq));
                p.speed_scale=1u;
                for(yaw=0u;;yaw=(uint8_t)(yaw+8u)){
                    p.yaw=yaw;
                    g_tspf_e1_host_all_segments=0u;
                    tsp_polar_renderer_reset();
                    tsp_polar_render(&p,pvs_map,(TSPColumn *)0);
                    g_tspf_e1_host_all_segments=1u;
                    tsp_polar_renderer_reset();
                    tsp_polar_render(&p,all_map,(TSPColumn *)0);
                    g_tspf_e1_host_all_segments=0u;
                    ++states;
                    for(mi=0u;mi<TSP_MAP_CELLS;++mi){
                        if(pvs_map[mi]!=all_map[mi]){
                            fprintf(stderr,
                                "FAIL: PVS false-negative/different owner x=%.2f y=%.2f yaw=%u cell=%u pvs=%04x all=%04x\n",
                                xq/16.0,yq/16.0,(unsigned)yaw,(unsigned)mi,
                                (unsigned)pvs_map[mi],(unsigned)all_map[mi]);
                            return 2;
                        }
                    }
                    if(yaw==248u)break;
                }
            }
        }
        if(!states){die("PVS oracle sampled no walkable states");return 2;}
        printf("E1M1_PVS_ORACLE_PASS sampled_states=%lu\n",(unsigned long)states);
    }

    printf("E1M1_ROOM1_POLAR_TEST_PASS walls=%u active=%u changed=%u\n",
           (unsigned)walls,(unsigned)g_tspf_active_runs,(unsigned)changed);
    return 0;
}
