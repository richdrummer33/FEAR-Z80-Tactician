#if defined(__SDCC)
#pragma bank 1
#endif

#include "fx.h"

static int16_t fx_abs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

uint16_t fx_lfsr16(uint16_t state) {
    uint16_t lsb;
    if (!state) state = 0xACE1u;
    lsb = (uint16_t)(state & 1u);
    state >>= 1;
    if (lsb) state ^= 0xB400u;
    return state;
}

void fx_projectile_init(FxProjectile *p, int16_t x, int16_t y, int16_t vx, int16_t vy,
                        uint8_t life, uint8_t seed) {
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

/* Every damping ratio in this subsystem is N/16. Avoid 32-bit multiply/divide
   helpers on Z80; FX velocities are deliberately bounded well inside 16-bit. */
static int16_t damp16(int16_t v, uint8_t numerator) {
    uint16_t mag;
    uint16_t scaled;
    if (!v || !numerator) return 0;
    if (v < 0) {
        mag = (uint16_t)(-v);
        scaled = (uint16_t)((mag * numerator) >> 4);
        return (int16_t)-(int16_t)scaled;
    }
    mag = (uint16_t)v;
    return (int16_t)((mag * numerator) >> 4);
}

void fx_debris_init(FxDebris *p, uint8_t size_class, int16_t x, int16_t y,
                    int16_t vx, int16_t vy, int16_t vz, uint8_t life, uint8_t seed) {
    p->x = x; p->y = y; p->z = 0;
    p->vx = vx; p->vy = vy; p->vz = vz;
    p->life = life; p->seed = seed; p->size_class = size_class;
    p->grounded = 0u; p->settle_age = 0u; p->active = 1u;
}

void fx_debris_tick(FxDebris *p, FxSolidFn solid, void *ctx) {
    int16_t nx, ny;
    uint8_t restitution_num;
    uint8_t drag_num;
    if (!p->active) return;

    restitution_num = p->size_class == FX_DEBRIS_CHUNK ? 11u : 7u;
    drag_num = p->grounded ? (p->size_class == FX_DEBRIS_CHUNK ? 13u : 10u)
                           : (p->size_class == FX_DEBRIS_CHUNK ? 15u : 13u);

    nx = (int16_t)(p->x + p->vx);
    if (solid && solid((int16_t)(nx >> FX_FP_SHIFT), (int16_t)(p->y >> FX_FP_SHIFT), ctx))
        p->vx = (int16_t)-damp16(p->vx, restitution_num);
    else p->x = nx;

    ny = (int16_t)(p->y + p->vy);
    if (solid && solid((int16_t)(p->x >> FX_FP_SHIFT), (int16_t)(ny >> FX_FP_SHIFT), ctx))
        p->vy = (int16_t)-damp16(p->vy, restitution_num);
    else p->y = ny;

    if (!p->grounded) {
        p->z = (int16_t)(p->z + p->vz);
        p->vz = (int16_t)(p->vz - 3);
        if (p->z <= 0 && p->vz < 0) {
            p->z = 0;
            if (p->size_class == FX_DEBRIS_CHUNK && p->vz < -10) {
                p->vz = (int16_t)-damp16(p->vz, 5u);
                p->vx = damp16(p->vx, 13u);
                p->vy = damp16(p->vy, 13u);
            } else {
                p->vz = 0;
                p->grounded = 1u;
            }
        }
    }

    p->vx = damp16(p->vx, drag_num);
    p->vy = damp16(p->vy, drag_num);

    if (p->life) --p->life;
    if (p->grounded && fx_abs16(p->vx) < 2 && fx_abs16(p->vy) < 2) {
        p->vx = p->vy = 0;
        if (p->settle_age < 255u) ++p->settle_age;
    } else p->settle_age = 0u;

    if (!p->life || p->settle_age > 24u) p->active = 0u;
}
