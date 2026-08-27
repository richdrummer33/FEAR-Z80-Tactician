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

static void test_q4_camera_step_and_accel_decel_path(void) {
    TSState s;
    int16_t cam_x,cam_z,prev_cam_x,prev_cam_z;
    int16_t prev_x,dx;
    unsigned i,moving_frames=0u,camera_response_frames=0u;
    uint16_t min_nonzero_step=0xffffu;

    /* Exact minimum representable translation: 1/16 world unit per sample.
     * This catches the original whole-unit camera truncation directly. */
    ts_reset(&s);
    s.yaw=0u;
    ts_debug_transform_vertex_q4(&s,0u,&prev_cam_x,&prev_cam_z);
    for(i=0u;i<16u;++i) {
        s.x_q4=(int16_t)(s.x_q4+1);
        ts_debug_transform_vertex_q4(&s,0u,&cam_x,&cam_z);
        assert(cam_x==prev_cam_x);
        assert(cam_z==(int16_t)(prev_cam_z-1));
        prev_cam_x=cam_x;
        prev_cam_z=cam_z;
    }

    /* Normal control path: accelerate from rest, then release and decelerate.
     * The early motion naturally contains 1-Q4 position deltas. Every nonzero
     * tracked-player movement in this interval must reach the camera transform. */
    ts_reset(&s);
    ts_debug_transform_vertex_q4(&s,0u,&prev_cam_x,&prev_cam_z);
    for(i=0u;i<24u;++i) {
        uint8_t input=(i<12u)?TS_INPUT_UP:0u;
        prev_x=s.x_q4;
        ts_step(&s,input);
        ts_debug_transform_vertex_q4(&s,0u,&cam_x,&cam_z);
        dx=(int16_t)(s.x_q4-prev_x);
        if(dx!=0) {
            uint16_t mag=(uint16_t)(dx<0?-dx:dx);
            ++moving_frames;
            if(mag<min_nonzero_step) min_nonzero_step=mag;
            if(cam_x!=prev_cam_x || cam_z!=prev_cam_z) ++camera_response_frames;
            assert(cam_x!=prev_cam_x || cam_z!=prev_cam_z);
        }
        prev_cam_x=cam_x;
        prev_cam_z=cam_z;
    }
    assert(s.speed_q4==0);
    assert(min_nonzero_step==1u);
    assert(moving_frames==camera_response_frames);
    printf("camera Q4: direct=16/16 min-motion-step=%u/16 response=%u/%u accel-decel-speed=%d\n",
           (unsigned)min_nonzero_step,camera_response_frames,moving_frames,(int)s.speed_q4);
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
    test_q4_camera_step_and_accel_decel_path();
    test_renderer_invariants_and_edge_vectors();
    test_open_arch_has_near_header_and_far_geometry();
    puts("tilesector tests: ok");
    return 0;
}
