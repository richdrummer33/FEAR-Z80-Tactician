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

    e1_room1_reset(&s);
    assert((s.x_q4>>4)==22 && (s.y_q4>>4)==52);
    assert((s.z_q4>>4)==19);

    assert(floor_z(22,52)==14);
    assert(floor_z(42,52)==12);
    assert(floor_z(46,52)==10);
    assert(floor_z(50,52)==8);
    assert(floor_z(54,52)==6);
    assert(floor_z(58,52)==4);
    assert(floor_z(62,52)==2);
    assert(floor_z(70,52)==0);

    assert(!e1_room1_is_walkable_q4((int16_t)(60<<4),(int16_t)(68<<4)));
    assert(!e1_room1_is_walkable_q4((int16_t)(60<<4),(int16_t)(36<<4)));
    assert(e1_room1_is_walkable_q4((int16_t)(96<<4),(int16_t)(52<<4)));
    assert(!e1_room1_is_walkable_q4((int16_t)(114<<4),(int16_t)(52<<4)));

    for(i=0u;i<100u;++i)e1_room1_step(&s,E1_INPUT_UP);
    assert((s.x_q4>>4)>30);

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
    assert(walls>0u);
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
