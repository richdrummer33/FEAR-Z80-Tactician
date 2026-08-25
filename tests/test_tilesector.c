#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "tilesector_core.h"

static void test_accel_takeover_strafe_and_speed_cycle(void) {
    TSState s;
    unsigned i;
    int16_t x0,y0;
    ts_reset(&s);
    assert(!s.manual);
    assert(s.speed_scale == 1u);
    for (i=0;i<5u;++i) ts_step(&s, TS_INPUT_SPEED);
    assert(s.speed_scale == 1u);
    ts_step(&s, TS_INPUT_SPEED);
    assert(s.speed_scale == 2u);

    ts_reset(&s);
    ts_step(&s, TS_INPUT_UP);
    assert(s.manual);
    for (i=0;i<40u;++i) ts_step(&s, TS_INPUT_UP);
    assert(s.speed_q4 == 192);
    for (i=0;i<40u;++i) ts_step(&s, 0u);
    assert(s.speed_q4 == 0);

    ts_reset(&s);
    x0=s.x_q4; y0=s.y_q4;
    for (i=0;i<40u;++i) ts_step(&s, TS_INPUT_STRAFE_RIGHT);
    assert(s.manual);
    assert(s.strafe_q4 == 192);
    assert(s.y_q4 > y0);
    assert(s.x_q4 == x0);
}

static void test_renderer_invariants_and_edge_vectors(void) {
    TSState s;
    uint16_t map[TS_MAP_CELLS];
    TSColumn cols[TS_COLS];
    unsigned f,c,i;
    unsigned visible=0u, sloped=0u, attrs=0u;
    ts_reset(&s);
    for (f=0;f<420u;++f) {
        ts_step(&s,0u);
        assert(ts_is_walkable_q4(s.x_q4,s.y_q4));
        ts_build_tilemap(&s,map,cols);
        for (i=0;i<TS_MAP_CELLS;++i) {
            assert((map[i]&TS_TILE_ID_MASK)<TS_GENERATED_TILE_COUNT);
            if(map[i]&(TS_ATTR_FLIPY|TS_ATTR_PALETTE)) ++attrs;
        }
        for (c=0;c<TS_COLS;++c) {
            if(cols[c].wall_id!=TS_NO_WALL) {
                ++visible;
                assert(cols[c].top<=cols[c].bottom);
                assert(cols[c].bottom<144u);
                if(cols[c].top_step||cols[c].bottom_step) ++sloped;
            }
        }
    }
    assert(visible>100u);
    assert(sloped>10u);
    assert(attrs>10u);
}

static void test_open_arch_has_near_header_and_far_geometry(void) {
    TSState s;
    uint16_t map[TS_MAP_CELLS];
    TSColumn cols[TS_COLS];
    unsigned i,wallish=0u;
    ts_reset(&s);
    for(i=0;i<70u;++i) ts_step(&s,0u);
    ts_build_tilemap(&s,map,cols);
    for(i=0;i<TS_ROWS;++i) {
        uint16_t t=map[i*TS_COLS+10u]&TS_TILE_ID_MASK;
        if(t>=TS_TILE_FULL_BASE) ++wallish;
    }
    assert(wallish>=2u);
}

int main(void) {
    test_accel_takeover_strafe_and_speed_cycle();
    test_renderer_invariants_and_edge_vectors();
    test_open_arch_has_near_header_and_far_geometry();
    puts("tilesector tests: ok");
    return 0;
}
