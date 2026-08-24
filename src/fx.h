#ifndef GG_CQB_FX_H
#define GG_CQB_FX_H

#include <stdint.h>

#define FX_TILE_BYTES 32u
#define FX_FP_SHIFT 4u
#define FX_FP_ONE (1 << FX_FP_SHIFT)
#define FX_MAX_DEBRIS 16u

#define FX_DEBRIS_SMALL 0u
#define FX_DEBRIS_CHUNK 1u

typedef struct {
    int16_t x, y;
    int16_t vx, vy;
    uint8_t life;
    uint8_t seed;
    uint8_t active;
} FxProjectile;

typedef struct {
    int16_t x, y;
    int16_t z;
    int16_t vx, vy;
    int16_t vz;
    uint8_t life;
    uint8_t seed;
    uint8_t size_class;
    uint8_t grounded;
    uint8_t settle_age;
    uint8_t active;
} FxDebris;

typedef uint8_t (*FxSolidFn)(int16_t px, int16_t py, void *ctx);

/* Drawing helpers live with the GG renderer on target (bank 3). */
void fx_tile_clear(uint8_t *tile);
void fx_tile_set_pixel(uint8_t *tile, uint8_t x, uint8_t y, uint8_t color);
uint8_t fx_tile_get_pixel(const uint8_t *tile, uint8_t x, uint8_t y);
void fx_tile_draw_line(uint8_t *tile, int8_t x0, int8_t y0, int8_t x1, int8_t y1, uint8_t color);
void fx_tile_draw_ring(uint8_t *tile, uint8_t radius, uint8_t color, uint8_t phase);
void fx_tile_draw_ring16_quadrant(uint8_t *tile, uint8_t quadrant, uint8_t radius, uint8_t color, uint8_t phase);
void fx_tile_draw_tracer(uint8_t *tile, int8_t dx, int8_t dy, uint8_t phase, uint8_t seed,
                         uint8_t head_color, uint8_t tail_color);
void fx_tile_apply_highbit_glow(uint8_t *tile, uint8_t x, uint8_t y);
uint8_t fx_dither_on(uint8_t x, uint8_t y, uint8_t phase, uint8_t numerator);

/* Timeline / physics helpers live with vfx_lab on target (bank 1). */
uint16_t fx_lfsr16(uint16_t state);
void fx_projectile_init(FxProjectile *p, int16_t x, int16_t y, int16_t vx, int16_t vy,
                        uint8_t life, uint8_t seed);
void fx_projectile_tick(FxProjectile *p);
void fx_debris_init(FxDebris *p, uint8_t size_class, int16_t x, int16_t y,
                    int16_t vx, int16_t vy, int16_t vz, uint8_t life, uint8_t seed);
void fx_debris_tick(FxDebris *p, FxSolidFn solid, void *ctx);

#endif
