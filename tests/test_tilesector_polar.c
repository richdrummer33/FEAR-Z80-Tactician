#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tilesector_polar.h"

int main(void){
    TSPState s; TSPColumn cols[TSP_COLS]; uint16_t map[TSP_MAP_CELLS];
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
