#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tilesector_polar.h"
#include "tilesector_world_module.h"

int main(void){
    TSPState s; TSPColumn cols[TSP_COLS]; uint16_t map[TSP_MAP_CELLS];
    uint16_t low[TSP_MAP_CELLS],high[TSP_MAP_CELLS];
    unsigned vertical_changed=0u;
    uint16_t k;

    /* Elevation contract: ordinary floor keeps the historical z=16 eye;
     * Room B is the existing four-unit raised-floor/riser proof. */
    tsp_reset(&s);
    if(s.z_q4!=TSP_EYE_HEIGHT_Q4){fprintf(stderr,"bad reset eye z=%d\n",s.z_q4);return 10;}
    if(tsp_floor_z_q4(96<<4,50<<4)!=0){fprintf(stderr,"connector floor z wrong\n");return 11;}
    if(tsp_floor_z_q4(140<<4,50<<4)!=TSP_ROOM_B_FLOOR_Z_Q4){fprintf(stderr,"raised floor z wrong\n");return 12;}

    /* Same XY/yaw, camera four units higher: the final name table must change.
     * This catches a renderer that stores Z but forgets to project with it. */
    memset(&s,0,sizeof(s));s.x_q4=96<<4;s.y_q4=50<<4;s.yaw=0u;s.speed_scale=1u;
    s.z_q4=TSP_EYE_HEIGHT_Q4;
    memset(low,0,sizeof(low));tsp_polar_renderer_reset();tsp_polar_render(&s,low,(TSPColumn *)0);
    s.z_q4=(int16_t)(TSP_EYE_HEIGHT_Q4+TSP_MODULE_STAIR_RISE_Q4);
    memset(high,0,sizeof(high));tsp_polar_renderer_reset();tsp_polar_render(&s,high,(TSPColumn *)0);
    for(k=0u;k<TSP_MAP_CELLS;++k)if(low[k]!=high[k])++vertical_changed;
    if(!vertical_changed){fprintf(stderr,"camera Z produced no projected change\n");return 13;}

    /* Four right-handed quarter-stair blocks are one full spiral revolution:
     * heading returns to its start while floor height rises sixteen units.
     * That is the compositional primitive used for a self-occluding stairwell. */
    {
        uint8_t dir=0u,i;int16_t dz,total=0;
        for(i=0u;i<4u;++i){
            if(!tsp_module_exit(TSP_MODULE_STAIR_QUARTER_UP_RIGHT,dir,0u,&dir,&dz))return 14;
            total=(int16_t)(total+dz);
        }
        if(dir!=0u||total!=(int16_t)(4*TSP_MODULE_STAIR_RISE_Q4)){
            fprintf(stderr,"quarter-stair composition failed dir=%u dz=%d\n",dir,total);return 15;
        }
    }

    /* Deterministic grammar must actually exercise ordinary, split and
     * vertical modules, and allow a caller to prohibit vertical generation. */
    {
        unsigned straight=0u,turn=0u,split=0u,vertical=0u;
        for(k=0u;k<256u;++k){
            TSPModuleKind a=tsp_module_choose(0x4a31u,k,1u);
            TSPModuleKind b=tsp_module_choose(0x4a31u,k,1u);
            if(a!=b){fprintf(stderr,"module generator nondeterministic\n");return 16;}
            if(a==TSP_MODULE_HALL_STRAIGHT)++straight;
            else if(a==TSP_MODULE_TURN_LEFT||a==TSP_MODULE_TURN_RIGHT)++turn;
            else if(a==TSP_MODULE_ROOM_SPLIT||a==TSP_MODULE_T_SPLIT)++split;
            else ++vertical;
            a=tsp_module_choose(0x4a31u,k,0u);
            if(a>=TSP_MODULE_STAIR_QUARTER_UP_LEFT){fprintf(stderr,"vertical module escaped allow_vertical=0\n");return 17;}
        }
        if(!straight||!turn||!split||!vertical){fprintf(stderr,"module grammar coverage failed\n");return 18;}
    }
    unsigned long runs=0, sels=0, touched=0; unsigned maxrun=0,maxsel=0,maxtouch=0; int f; unsigned i;
    memset(map,0,sizeof(map));tsp_reset(&s);tsp_polar_renderer_reset();
    for(f=0;f<360;++f){
        tsp_step(&s,0);tsp_polar_render(&s,map,cols);
        runs+=g_tspf_active_runs;sels+=g_tspf_selector_tests;touched+=g_tspf_touched_cells;
        if(g_tspf_active_runs>maxrun) maxrun=g_tspf_active_runs;
        if(g_tspf_selector_tests>maxsel) maxsel=g_tspf_selector_tests;
        if(g_tspf_touched_cells>maxtouch) maxtouch=g_tspf_touched_cells;
        if(g_tspf_active_runs>20u||g_tspf_touched_cells>360u){fprintf(stderr,"counter overflow frame %d\n",f);return 2;}
        for(i=0;i<TSP_MAP_CELLS;++i){if((map[i]&TSP_TILE_ID_MASK)>=TSP_GENERATED_TILE_COUNT){fprintf(stderr,"bad tile %u frame %d cell %d\n",map[i]&TSP_TILE_ID_MASK,f,i);return 3;}}
    }
    printf("frames=360 avg_runs=%.2f max_runs=%u avg_selectors=%.2f max_selectors=%u avg_touched=%.2f max_touched=%u final=(%.2f,%.2f) yaw=%u\n",
      runs/360.0,maxrun,sels/360.0,maxsel,touched/360.0,maxtouch,s.x_q4/16.0,s.y_q4/16.0,s.yaw);
    return 0;
}
