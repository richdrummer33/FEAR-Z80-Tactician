#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "tilesector_core.h"

static void test_accel_and_takeover(void) {
    TSState s;
    uint16_t map[TS_MAP_CELLS];
    TSColumn cols[TS_COLS];
    unsigned i;
    ts_reset(&s);
    assert(!s.manual);
    ts_step(&s, TS_INPUT_UP);
    assert(s.manual);
    for (i = 0; i < 40; ++i) ts_step(&s, TS_INPUT_UP);
    assert(s.speed_q4 == 192);
    for (i = 0; i < 40; ++i) ts_step(&s, 0);
    assert(s.speed_q4 == 0);
    ts_build_tilemap(&s, map, cols);
    for (i = 0; i < TS_MAP_CELLS; ++i) assert(map[i] < TS_GENERATED_TILE_COUNT);
}

static void test_demo_renderer_invariants(void) {
    TSState s;
    uint16_t map[TS_MAP_CELLS];
    TSColumn cols[TS_COLS];
    unsigned f, c;
    unsigned visible_columns = 0;
    ts_reset(&s);
    for (f = 0; f < 800; ++f) {
        ts_step(&s, 0);
        assert(ts_is_walkable_q8(s.x_q8, s.y_q8));
        ts_build_tilemap(&s, map, cols);
        for (c = 0; c < TS_COLS; ++c) {
            if (cols[c].wall_id != 0xffu) {
                ++visible_columns;
                assert(cols[c].top <= cols[c].bottom);
                assert(cols[c].bottom < 144u);
            }
        }
    }
    assert(visible_columns > 100u);
}

int main(void) {
    test_accel_and_takeover();
    test_demo_renderer_invariants();
    puts("tilesector tests: ok");
    return 0;
}
