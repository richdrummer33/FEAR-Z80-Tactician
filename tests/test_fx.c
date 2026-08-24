#include <assert.h>
#include <stdio.h>
#include "fx.h"

typedef struct { int16_t wall_x; } TestMap;
static uint8_t solid_at(int16_t x, int16_t y, void *ctx) {
    TestMap *m = (TestMap *)ctx;
    (void)y;
    return x >= m->wall_x;
}

static unsigned count_nonzero(const uint8_t *t) {
    unsigned x,y,n=0;
    for(y=0;y<8;y++) for(x=0;x<8;x++) if(fx_tile_get_pixel(t,(uint8_t)x,(uint8_t)y)) ++n;
    return n;
}

int main(void) {
    uint8_t tile[32];
    unsigned x,y,n1=0,n2=0;
    FxProjectile p;
    FxDebris d;
    TestMap m={6};

    fx_tile_clear(tile);
    fx_tile_set_pixel(tile, 3, 5, 13);
    assert(fx_tile_get_pixel(tile,3,5)==13);
    assert(fx_tile_get_pixel(tile,4,5)==0);

    fx_tile_clear(tile);
    fx_tile_draw_line(tile,0,0,7,5,15);
    assert(count_nonzero(tile)>=8);

    for(y=0;y<8;y++) for(x=0;x<8;x++) {
        n1 += fx_dither_on((uint8_t)x,(uint8_t)y,0,1);
        n2 += fx_dither_on((uint8_t)x,(uint8_t)y,0,2);
    }
    assert(n1>=20 && n1<=22);
    assert(n2>=42 && n2<=44);

    fx_tile_draw_tracer(tile,6,3,1,7,15,14);
    assert(fx_tile_get_pixel(tile,4,4)==15);
    assert(count_nonzero(tile)>=3);

    fx_projectile_init(&p, 140*FX_FP_ONE, 70*FX_FP_ONE, 3*FX_FP_ONE, 1*FX_FP_ONE, 4, 1);
    fx_projectile_tick(&p);
    assert((p.x>>FX_FP_SHIFT)==143);
    assert((p.y>>FX_FP_SHIFT)==71);

    fx_debris_init(&d, FX_DEBRIS_CHUNK, 4*FX_FP_ONE, 4*FX_FP_ONE,
                   3*FX_FP_ONE, 0, 18, 100, 3);
    fx_debris_tick(&d, solid_at, &m);
    assert(d.vx < 0); /* hit x=7 wall and bounced */
    assert(d.z > 0);

    fx_debris_init(&d, FX_DEBRIS_SMALL, 2*FX_FP_ONE, 2*FX_FP_ONE,
                   FX_FP_ONE, 0, 8, 100, 4);
    for(x=0;x<40 && d.active;x++) fx_debris_tick(&d, 0, 0);
    assert(d.grounded || !d.active);

    puts("fx tests: ok");
    return 0;
}
