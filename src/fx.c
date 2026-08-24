#include "fx.h"

static int16_t fx_abs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

void fx_tile_clear(uint8_t *tile) {
    uint8_t i;
    for (i = 0u; i < FX_TILE_BYTES; ++i) tile[i] = 0u;
}

void fx_tile_set_pixel(uint8_t *tile, uint8_t x, uint8_t y, uint8_t color) {
    uint8_t plane;
    uint8_t bit;
    uint8_t *row;
    if (x >= 8u || y >= 8u) return;
    bit = (uint8_t)(0x80u >> x);
    row = tile + (uint16_t)y * 4u;
    for (plane = 0u; plane < 4u; ++plane) {
        if (color & (uint8_t)(1u << plane)) row[plane] |= bit;
        else row[plane] &= (uint8_t)~bit;
    }
}

uint8_t fx_tile_get_pixel(const uint8_t *tile, uint8_t x, uint8_t y) {
    uint8_t plane, color = 0u;
    uint8_t bit;
    const uint8_t *row;
    if (x >= 8u || y >= 8u) return 0u;
    bit = (uint8_t)(0x80u >> x);
    row = tile + (uint16_t)y * 4u;
    for (plane = 0u; plane < 4u; ++plane)
        if (row[plane] & bit) color |= (uint8_t)(1u << plane);
    return color;
}

void fx_tile_draw_line(uint8_t *tile, int8_t x0, int8_t y0, int8_t x1, int8_t y1, uint8_t color) {
    int8_t dx = (int8_t)fx_abs16((int16_t)x1 - x0);
    int8_t sx = x0 < x1 ? 1 : -1;
    int8_t dy = (int8_t)-fx_abs16((int16_t)y1 - y0);
    int8_t sy = y0 < y1 ? 1 : -1;
    int16_t err = (int16_t)dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < 8 && y0 >= 0 && y0 < 8)
            fx_tile_set_pixel(tile, (uint8_t)x0, (uint8_t)y0, color);
        if (x0 == x1 && y0 == y1) break;
        {
            int16_t e2 = (int16_t)(err << 1);
            if (e2 >= dy) { err = (int16_t)(err + dy); x0 = (int8_t)(x0 + sx); }
            if (e2 <= dx) { err = (int16_t)(err + dx); y0 = (int8_t)(y0 + sy); }
        }
    }
}

uint8_t fx_dither_on(uint8_t x, uint8_t y, uint8_t phase, uint8_t numerator) {
    /* Three interleaved phases. numerator=1 -> 1/3, numerator=2 -> 2/3. */
    uint8_t bucket = (uint8_t)((x + (uint8_t)(y << 1) + phase) % 3u);
    if (numerator >= 3u) return 1u;
    if (numerator == 0u) return 0u;
    return bucket < numerator;
}

void fx_tile_apply_highbit_glow(uint8_t *tile, uint8_t x, uint8_t y) {
    uint8_t c = fx_tile_get_pixel(tile, x, y);
    if (c >= 1u && c <= 4u) fx_tile_set_pixel(tile, x, y, (uint8_t)(c + 8u));
}

void fx_tile_draw_ring(uint8_t *tile, uint8_t radius, uint8_t color, uint8_t phase) {
    int8_t x, y;
    int16_t r2 = (int16_t)radius * radius;
    int16_t inner = radius > 0u ? (int16_t)(radius - 1u) * (radius - 1u) : 0;
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            int16_t dx = (int16_t)x - 3;
            int16_t dy = (int16_t)y - 3;
            int16_t d2 = (int16_t)(dx * dx + dy * dy);
            if (d2 <= r2 && d2 >= inner && fx_dither_on((uint8_t)x, (uint8_t)y, phase, 2u))
                fx_tile_set_pixel(tile, (uint8_t)x, (uint8_t)y, color);
        }
    }
}

void fx_tile_draw_tracer(uint8_t *tile, int8_t dx, int8_t dy, uint8_t phase, uint8_t seed,
                         uint8_t head_color, uint8_t tail_color) {
    int8_t x0 = 4, y0 = 4;
    int8_t x1 = (int8_t)(4 - dx), y1 = (int8_t)(4 - dy);
    int8_t adx = (int8_t)fx_abs16(dx), ady = (int8_t)fx_abs16(dy);
    int8_t steps = adx > ady ? adx : ady;
    int8_t i;
    fx_tile_clear(tile);
    if (steps < 1) steps = 1;
    for (i = 0; i <= steps; ++i) {
        int8_t x = (int8_t)(x0 + ((int16_t)(x1 - x0) * i) / steps);
        int8_t y = (int8_t)(y0 + ((int16_t)(y1 - y0) * i) / steps);
        uint8_t numer;
        if (x < 0 || x >= 8 || y < 0 || y >= 8) continue;
        if (i <= (steps / 3)) numer = 3u;
        else if (i <= ((steps * 2) / 3)) numer = 2u;
        else numer = 1u;
        if (fx_dither_on((uint8_t)x, (uint8_t)y, (uint8_t)(phase + seed), numer))
            fx_tile_set_pixel(tile, (uint8_t)x, (uint8_t)y, tail_color);
    }
    fx_tile_set_pixel(tile, 4u, 4u, head_color);
    if (4u < 7u) fx_tile_set_pixel(tile, 5u, 4u, head_color);
}

uint16_t fx_lfsr16(uint16_t state) {
    uint16_t lsb;
    if (!state) state = 0xACE1u;
    lsb = (uint16_t)(state & 1u);
    state >>= 1;
    if (lsb) state ^= 0xB400u;
    return state;
}

void fx_projectile_init(FxProjectile *p, int16_t x, int16_t y, int16_t vx, int16_t vy, uint8_t life, uint8_t seed) {
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->life = life; p->seed = seed; p->active = 1u;
}

void fx_projectile_tick(FxProjectile *p) {
    if (!p->active) return;
    p->x = (int16_t)(p->x + p->vx);
    p->y = (int16_t)(p->y + p->vy);
    if (p->life) --p->life;
    if (!p->life) p->active = 0u;
}

void fx_debris_init(FxDebris *p, uint8_t size_class, int16_t x, int16_t y,
                    int16_t vx, int16_t vy, int16_t vz, uint8_t life, uint8_t seed) {
    p->x = x; p->y = y; p->z = 0;
    p->vx = vx; p->vy = vy; p->vz = vz;
    p->life = life; p->seed = seed; p->size_class = size_class;
    p->grounded = 0u; p->active = 1u;
}

static int16_t damp(int16_t v, uint8_t num, uint8_t den) {
    return (int16_t)(((int32_t)v * num) / den);
}

void fx_debris_tick(FxDebris *p, FxSolidFn solid, void *ctx) {
    int16_t nx, ny;
    uint8_t restitution_num;
    uint8_t drag_num;
    if (!p->active) return;

    restitution_num = p->size_class == FX_DEBRIS_CHUNK ? 11u : 7u; /* /16 */
    drag_num = p->grounded ? (p->size_class == FX_DEBRIS_CHUNK ? 13u : 10u)
                           : (p->size_class == FX_DEBRIS_CHUNK ? 15u : 13u);

    nx = (int16_t)(p->x + p->vx);
    if (solid && solid((int16_t)(nx >> FX_FP_SHIFT), (int16_t)(p->y >> FX_FP_SHIFT), ctx))
        p->vx = (int16_t)-damp(p->vx, restitution_num, 16u);
    else p->x = nx;

    ny = (int16_t)(p->y + p->vy);
    if (solid && solid((int16_t)(p->x >> FX_FP_SHIFT), (int16_t)(ny >> FX_FP_SHIFT), ctx))
        p->vy = (int16_t)-damp(p->vy, restitution_num, 16u);
    else p->y = ny;

    if (!p->grounded) {
        p->z = (int16_t)(p->z + p->vz);
        p->vz = (int16_t)(p->vz - 3); /* hidden gravity */
        if (p->z <= 0 && p->vz < 0) {
            p->z = 0;
            if (p->size_class == FX_DEBRIS_CHUNK && p->vz < -10) {
                p->vz = (int16_t)-damp(p->vz, 5u, 16u);
                p->vx = damp(p->vx, 13u, 16u);
                p->vy = damp(p->vy, 13u, 16u);
            } else {
                p->vz = 0;
                p->grounded = 1u;
            }
        }
    }

    p->vx = damp(p->vx, drag_num, 16u);
    p->vy = damp(p->vy, drag_num, 16u);

    if (p->life) --p->life;
    if (!p->life || (p->grounded && fx_abs16(p->vx) < 2 && fx_abs16(p->vy) < 2)) p->active = 0u;
}
