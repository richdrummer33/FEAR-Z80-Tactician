#include <assert.h>
#include <stdio.h>
#include "fx.h"

typedef struct { int16_t wall_x; } TestMap;

static uint8_t solid_at(int16_t x, int16_t y, void *ctx) {
    TestMap *m = (TestMap *)ctx;
    (void)y;
    return x >= m->wall_x;
}

static unsigned count_nonzero(const uint8_t *tile) {
    unsigned x, y, n = 0u;
    for (y = 0u; y < 8u; ++y)
        for (x = 0u; x < 8u; ++x)
            if (fx_tile_get_pixel(tile, (uint8_t)x, (uint8_t)y)) ++n;
    return n;
}

int main(void) {
    uint8_t tile[32u];
    unsigned x, y, n1 = 0u, n2 = 0u;
    FxProjectile p;
    FxDebris d;
    TestMap m = {6};

    fx_tile_clear(tile);
    fx_tile_set_pixel(tile, 3u, 5u, 13u);
    assert(fx_tile_get_pixel(tile, 3u, 5u) == 13u);
    assert(fx_tile_get_pixel(tile, 4u, 5u) == 0u);

    fx_tile_clear(tile);
    fx_tile_draw_line(tile, 0, 0, 7, 5, 15u);
    assert(count_nonzero(tile) >= 8u);

    for (y = 0u; y < 8u; ++y) {
        for (x = 0u; x < 8u; ++x) {
            n1 += fx_dither_on((uint8_t)x, (uint8_t)y, 0u, 1u);
            n2 += fx_dither_on((uint8_t)x, (uint8_t)y, 0u, 2u);
        }
    }
    assert(n1 >= 20u && n1 <= 22u);
    assert(n2 >= 42u && n2 <= 44u);

    fx_tile_draw_tracer(tile, 6, 3, 1u, 7u, 15u, 14u);
    assert(fx_tile_get_pixel(tile, 4u, 4u) == 15u);
    assert(count_nonzero(tile) >= 3u);

    fx_tile_draw_ring16_quadrant(tile, 0u, 7u, 12u, 1u);
    assert(count_nonzero(tile) > 0u);

    fx_projectile_init(&p, 140 * FX_FP_ONE, 70 * FX_FP_ONE,
                       3 * FX_FP_ONE, FX_FP_ONE, 4u, 1u);
    fx_projectile_tick(&p);
    assert((p.x >> FX_FP_SHIFT) == 143);
    assert((p.y >> FX_FP_SHIFT) == 71);

    fx_debris_init(&d, FX_DEBRIS_CHUNK, 4 * FX_FP_ONE, 4 * FX_FP_ONE,
                   3 * FX_FP_ONE, 0, 18, 100u, 3u);
    fx_debris_tick(&d, solid_at, &m);
    assert(d.vx < 0);
    assert(d.z > 0);

    fx_debris_init(&d, FX_DEBRIS_SMALL, 2 * FX_FP_ONE, 2 * FX_FP_ONE,
                   FX_FP_ONE, 0, 8, 100u, 4u);
    for (x = 0u; x < 40u && d.active; ++x) fx_debris_tick(&d, 0, 0);
    assert(d.grounded || !d.active);

    puts("fx tests: ok");
    return 0;
}
