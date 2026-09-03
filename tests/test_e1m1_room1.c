#include <assert.h>
#include <stdio.h>
#include "e1m1_room1_core.h"

static int floor_z(int x,int y) {
    return e1_room1_floor_z_q4((int16_t)(x<<4),(int16_t)(y<<4))>>4;
}

int main(void) {
    E1Room1State s;
    uint16_t map[E1_MAP_CELLS];
    unsigned i,walls=0u,borders=0u;

    assert(floor_z(72,52)==0);
    assert(floor_z(66,40)==2);
    assert(floor_z(62,40)==4);
    assert(floor_z(58,40)==6);
    assert(floor_z(54,40)==8);
    assert(floor_z(50,40)==10);
    assert(floor_z(46,40)==12);
    assert(floor_z(30,40)==14);

    assert(!e1_room1_is_walkable_q4((int16_t)(56<<4),(int16_t)(60<<4)));
    assert(!e1_room1_is_walkable_q4((int16_t)(72<<4),(int16_t)(60<<4)));
    assert(e1_room1_is_walkable_q4((int16_t)(96<<4),(int16_t)(56<<4)));
    assert(!e1_room1_is_walkable_q4((int16_t)(113<<4),(int16_t)(56<<4)));

    e1_room1_reset(&s);
    e1_room1_render(&s,map);

    for(i=0u;i<E1_MAP_CELLS;++i) {
        uint16_t id=map[i]&E1_TILE_ID_MASK;
        if(id>=E1_TILE_FULL_BASE) {
            unsigned rel=id-E1_TILE_FULL_BASE;
            unsigned border=rel%E1_BORDER_COUNT;
            ++walls;
            if(border)++borders;
        }
    }

    assert(walls>35u);
    assert(borders>0u);

    {
        uint8_t yaw0=s.yaw;
        for(i=0u;i<20u;++i)e1_room1_step(&s,E1_INPUT_RIGHT);
        assert(s.yaw!=yaw0);
    }

    printf("E1M1_ROOM1_TEST_PASS surfaces=%u wall_cells=%u bordered=%u pos=%d,%d,%d yaw=%u\n",
           e1_room1_surface_count(),walls,borders,
           s.x_q4>>4,s.y_q4>>4,s.z_q4>>4,s.yaw);
    return 0;
}
