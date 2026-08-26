#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

/* Stage 1 regression: the render camera must respond before the player crosses
 * a whole authored world unit. The old renderer truncated x_q4/y_q4 before the
 * camera transform, so all 15 fractional poses produced the same frame. */
static void test_fractional_camera_reaches_renderer(void) {
    TSState s;
    TSColumn cols[TS_COLS];
    uint16_t base[TS_MAP_CELLS], map[TS_MAP_CELLS];
    unsigned q;
    int changed_at=0;
    ts_reset(&s);
    s.y_q4=(int16_t)(24<<4); /* close enough to make sub-unit parallax visible */
    ts_build_tilemap(&s,base,cols);
    for(q=1u;q<16u;++q) {
        s.y_q4=(int16_t)((24<<4)+(int16_t)q);
        ts_build_tilemap(&s,map,cols);
        if(memcmp(base,map,sizeof(base))!=0) { changed_at=(int)q; break; }
    }
    assert(changed_at>0 && changed_at<16);
}

static void test_renderer_invariants_and_edge_vectors(void) {
    TSState s;
    uint16_t map[TS_MAP_CELLS];
    TSColumn cols[TS_COLS];
    unsigned f,c,i;
    unsigned visible=0u, sloped=0u, attrs=0u, continuous=0u;
    ts_reset(&s);
    assert(TS_GENERATED_TILE_COUNT < 448u); /* keep below GG name-table region */
    for (f=0;f<420u;++f) {
        ts_step(&s,0u);
        assert(ts_is_walkable_q4(s.x_q4,s.y_q4));
        ts_build_tilemap(&s,map,cols);
        assert(g_ts_render_stage==0u);
        for (i=0;i<TS_MAP_CELLS;++i) {
            assert((map[i]&TS_TILE_ID_MASK)<TS_GENERATED_TILE_COUNT);
            if(map[i]&(TS_ATTR_FLIPX|TS_ATTR_FLIPY|TS_ATTR_PALETTE)) ++attrs;
        }
        for (c=0;c<TS_COLS;++c) {
            if(cols[c].wall_id!=TS_NO_WALL) {
                ++visible;
                assert(cols[c].top<=cols[c].bottom);
                assert(cols[c].bottom<144u);
                assert(cols[c].top_step>=-7 && cols[c].top_step<=7);
                assert(cols[c].bottom_step>=-7 && cols[c].bottom_step<=7);
                if(cols[c].top_step||cols[c].bottom_step) ++sloped;
                if(c+1u<TS_COLS && cols[c+1u].wall_id==cols[c].wall_id &&
                   cols[c+1u].wall_id!=TS_NO_WALL) {
                    int16_t next=(int16_t)cols[c].top+(int16_t)cols[c].top_step;
                    int16_t d=next-(int16_t)cols[c+1u].top;
                    if(d<0)d=-d;
                    if(cols[c].top>0u && cols[c].top<143u && cols[c+1u].top>0u && cols[c+1u].top<143u) {
                        assert(d<=1); /* connected edge, allowing metadata rounding */
                        ++continuous;
                    }
                }
            }
        }
    }
    assert(visible>100u);
    assert(sloped>10u);
    assert(attrs>10u);
    assert(continuous>20u);
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
    test_fractional_camera_reaches_renderer();
    test_renderer_invariants_and_edge_vectors();
    test_open_arch_has_near_header_and_far_geometry();
    puts("tilesector tests: ok");
    return 0;
}
