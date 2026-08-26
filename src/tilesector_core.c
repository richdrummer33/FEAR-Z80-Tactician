#include "tilesector_core.h"
#include <string.h>

#define TS_NEAR_Z 10
#define TS_FAR_Z 127
#define TS_NEAR_Z_Q4 (TS_NEAR_Z << 4)
#define TS_FAR_Z_Q4  (TS_FAR_Z << 4)
#define TS_HORIZON 72
#define TS_RUN_SPEED_Q4 192
#define TS_ACCEL_Q4 6
#define TS_MANUAL_TURN_Q4 48
#define TS_MANUAL_TURN_ACCEL_Q4 16
#define TS_AUTO_TURN_Q4 40
#define TS_AUTO_TURN_ACCEL_Q4 4
#define TS_SEGMENTS 17u
#define TS_SECTORS 3u
#define TS_PORTALS 2u
#define TS_MAX_PORTAL_DEPTH 3u
#define TS_NO_PORTAL 0xffu

/* Stage markers are intentionally tiny writes. Gearsystem watches this symbol
 * instruction-by-instruction without perturbing the ROM's timing. */
volatile uint8_t g_ts_render_stage;

typedef struct { int16_t x, y; } TSVertex;
typedef enum {
    TS_PROFILE_FULL = 0,
    TS_PROFILE_LINTEL,
    TS_PROFILE_RAISED_FULL,
    TS_PROFILE_RISER
} TSProfile;
typedef struct {
    uint8_t a, b;
    int8_t shade_bias;
    uint8_t profile;
} TSSegment;
typedef struct {
    uint16_t frames;
    uint8_t yaw;
    int8_t throttle;
} TSDemoPhase;
typedef struct {
    uint8_t sector_a, sector_b;
    uint8_t lintel_seg;
    uint8_t riser_seg;
} TSPortal;
typedef struct {
    int8_t c0, c1;
    int16_t inv_q6;
    int16_t step_q6;
    int8_t original_c0, original_c1;
} TSProjectedSpan;

/* Two rooms, a short connector and two open arch planes. Room B deliberately
 * has a zig-zag far wall so perspective-edge continuity is easy to inspect. */
static const TSVertex k_vertices[] = {
    { 16, 16 }, { 80, 16 }, { 80, 36 }, { 80, 64 }, { 80, 80 }, { 16, 80 },
    {112, 36 }, {112, 64 }, {112, 14 }, {112, 84 },
    {136,  6 }, {154, 20 }, {176, 10 }, {176, 84 }
};
#define TS_VERTICES ((uint8_t)(sizeof(k_vertices) / sizeof(k_vertices[0])))

static const TSSegment k_segments[TS_SEGMENTS] = {
    {0,1, 0,TS_PROFILE_FULL}, {1,2, 0,TS_PROFILE_FULL},
    {3,4, 0,TS_PROFILE_FULL}, {4,5, 0,TS_PROFILE_FULL}, {5,0, 0,TS_PROFILE_FULL},
    {2,6,-1,TS_PROFILE_FULL}, {7,3,-1,TS_PROFILE_FULL},
    {8,6, 0,TS_PROFILE_RAISED_FULL}, {7,9, 0,TS_PROFILE_RAISED_FULL},
    {8,10,1,TS_PROFILE_RAISED_FULL}, {10,11,0,TS_PROFILE_RAISED_FULL},
    {11,12,1,TS_PROFILE_RAISED_FULL}, {12,13,0,TS_PROFILE_RAISED_FULL},
    {13,9,0,TS_PROFILE_RAISED_FULL},
    {2,3, 1,TS_PROFILE_LINTEL},
    {6,7, 1,TS_PROFILE_LINTEL},
    {6,7,-1,TS_PROFILE_RISER}
};

/* Solid/occluding walls owned by each sector. Portal faces are deliberately
 * separate: topology decides whether their aperture may recurse outward. */
static const uint8_t k_sector_count[TS_SECTORS] = {5u, 2u, 7u};
static const uint8_t k_sector_segments[TS_SECTORS][7] = {
    {0u,1u,2u,3u,4u,0xffu,0xffu},
    {5u,6u,0xffu,0xffu,0xffu,0xffu,0xffu},
    {7u,8u,9u,10u,11u,12u,13u}
};
static const TSPortal k_portals[TS_PORTALS] = {
    {0u,1u,14u,TS_NO_WALL},
    {1u,2u,15u,16u}
};

/* Direct sector -> portal adjacency avoids rescanning every portal at every
 * recursion level. The demo topology is a chain: room A <-> connector <-> room B. */
static const uint8_t k_sector_portal_count[TS_SECTORS] = {1u,2u,1u};
static const uint8_t k_sector_portals[TS_SECTORS][2] = {
    {0u,TS_NO_PORTAL}, {0u,1u}, {1u,TS_NO_PORTAL}
};

/* Recorded player path. */
static const TSDemoPhase k_demo[] = {
    {145u,   0u, 1},
    { 10u, 248u, 1}, { 10u, 236u, 1}, { 12u, 224u, 1}, { 12u, 212u, 1},
    { 12u, 224u, 1}, { 10u, 236u, 1}, { 10u, 248u, 1}, { 16u,   0u, 1},
    { 36u,   0u, 0}
};
#define TS_DEMO_PHASES ((uint8_t)(sizeof(k_demo) / sizeof(k_demo[0])))

static const int8_t k_sin[256] = {
0,3,6,9,12,16,19,22,25,28,31,34,37,40,43,46,
49,51,54,57,60,63,65,68,71,73,76,78,81,83,85,88,
90,92,94,96,98,100,102,104,106,107,109,111,112,113,115,116,
117,118,120,121,122,122,123,124,125,125,126,126,126,127,127,127,
127,127,127,127,126,126,126,125,125,124,123,122,122,121,120,118,
117,116,115,113,112,111,109,107,106,104,102,100,98,96,94,92,
90,88,85,83,81,78,76,73,71,68,65,63,60,57,54,51,
49,46,43,40,37,34,31,28,25,22,19,16,12,9,6,3,
0,-3,-6,-9,-12,-16,-19,-22,-25,-28,-31,-34,-37,-40,-43,-46,
-49,-51,-54,-57,-60,-63,-65,-68,-71,-73,-76,-78,-81,-83,-85,-88,
-90,-92,-94,-96,-98,-100,-102,-104,-106,-107,-109,-111,-112,-113,-115,-116,
-117,-118,-120,-121,-122,-122,-123,-124,-125,-125,-126,-126,-126,-127,-127,-127,
-127,-127,-127,-127,-126,-126,-126,-125,-125,-124,-123,-122,-122,-121,-120,-118,
-117,-116,-115,-113,-112,-111,-109,-107,-106,-104,-102,-100,-98,-96,-94,-92,
-90,-88,-85,-83,-81,-78,-76,-73,-71,-68,-65,-63,-60,-57,-54,-51,
-49,-46,-43,-40,-37,-34,-31,-28,-25,-22,-19,-16,-12,-9,-6,-3
};

/* 2560/z-ish reciprocal/projection scale. Runtime perspective remains LUT
 * driven; fractional Z linearly interpolates adjacent entries. */
static const uint8_t k_invz[128] = {
255,255,255,255,255,255,255,255,255,255,255,233,213,197,183,171,
160,151,142,135,128,122,116,111,107,102,98,95,91,88,85,83,
80,78,75,73,71,69,67,66,64,62,61,60,58,57,56,54,
53,52,51,50,49,48,47,47,46,45,44,43,43,42,41,41,
40,39,39,38,38,37,37,36,36,35,35,34,34,33,33,32,
32,32,31,31,30,30,30,29,29,29,28,28,28,28,27,27,
27,26,26,26,26,25,25,25,25,24,24,24,24,23,23,23,
23,23,22,22,22,22,22,22,21,21,21,21,21,20,20,20
};

static const uint8_t k_span_recip_q6[64] = {
0,64,32,21,16,13,11,9,8,7,6,6,5,5,5,4,
4,4,4,3,3,3,3,3,3,3,2,2,2,2,2,2,
2,2,2,2,2,2,2,2,2,2,2,1,1,1,1,1,
1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
};

/* Avoid row*20 in hot tile loops. */
static const uint16_t k_row_base[TS_ROWS] = {
    0u,20u,40u,60u,80u,100u,120u,140u,160u,
    180u,200u,220u,240u,260u,280u,300u,320u,340u
};

/* Camera-space coordinates preserve Q4 precision through projection. */
static int16_t g_cam_x_q4[TS_VERTICES];
static int16_t g_cam_z_q4[TS_VERTICES];

/* Projection is invariant for a world segment during one rendered frame. Cache
 * the 17 tiny spans so portals/candidate passes never redo clipping, reciprocal
 * interpolation and perspective projection for the same segment. */
static TSProjectedSpan g_span_cache[TS_SEGMENTS];
static uint8_t g_span_state[TS_SEGMENTS]; /* 0 unknown, 1 rejected, 2 visible */

/* Per-sector nearest solid candidate for each 8px screen column. This replaces
 * the old 20x18 generalized Z field: no vertical-cell depth disputes. */
static uint8_t g_best_seg[TS_COLS];
static uint8_t g_best_inv[TS_COLS];
static uint8_t g_best_border[TS_COLS];
static int16_t g_best_inv_l_q6[TS_COLS];
static int16_t g_best_inv_r_q6[TS_COLS];

/* One contiguous visible vertical aperture per coarse screen column, per
 * recursion depth. Child sectors receive a clipped copy through a portal. */
static uint8_t g_clip_top[TS_MAX_PORTAL_DEPTH][TS_COLS];
static uint8_t g_clip_bottom[TS_MAX_PORTAL_DEPTH][TS_COLS];
static uint16_t g_base_map[TS_MAP_CELLS];
static uint8_t g_base_map_ready;

static int8_t clamp_s8(int16_t v, int8_t lo, int8_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int8_t)v;
}
static uint8_t clamp_u8(int16_t v, uint8_t hi) {
    if (v < 0) return 0u;
    if (v > hi) return hi;
    return (uint8_t)v;
}
static int8_t yaw_error(uint8_t target, uint8_t yaw) { return (int8_t)(target - yaw); }

static int16_t slew_i16(int16_t cur, int16_t target, int16_t step) {
    if (cur < target) {
        cur = (int16_t)(cur + step);
        return cur > target ? target : cur;
    }
    if (cur > target) {
        cur = (int16_t)(cur - step);
        return cur < target ? target : cur;
    }
    return cur;
}

/* Scale 1..5 by additions: cheaper and predictable on Z80. */
static int16_t scale_small(int16_t v, uint8_t scale) {
    int16_t out = v;
    if (scale >= 2u) out = (int16_t)(out + v);
    if (scale >= 3u) out = (int16_t)(out + v);
    if (scale >= 4u) out = (int16_t)(out + v);
    if (scale >= 5u) out = (int16_t)(out + v);
    return out;
}

uint8_t ts_is_walkable_q4(int16_t x_q4, int16_t y_q4) {
    int16_t x = (int16_t)(x_q4 >> 4);
    int16_t y = (int16_t)(y_q4 >> 4);
    if (x >= 20 && x <= 76 && y >= 20 && y <= 76) return 1u;
    if (x >= 74 && x <= 116 && y >= 40 && y <= 60) return 1u;
    if (x >= 112 && x <= 172 && y >= 20 && y <= 78) return 1u;
    return 0u;
}

void ts_reset(TSState *s) {
    s->x_q4 = (int16_t)(32 << 4);
    s->y_q4 = (int16_t)(48 << 4);
    s->yaw = 0u;
    s->speed_q4 = 0;
    s->strafe_q4 = 0;
    s->turn_q4 = 0;
    s->speed_scale = 1u;
    s->manual = 0u;
    s->demo_phase = 0u;
    s->demo_ticks = 0u;
}

static void apply_motion(TSState *s, int8_t throttle, int8_t strafe,
                         uint8_t target_yaw, uint8_t manual_turn) {
    int16_t target_speed = (int16_t)throttle * TS_RUN_SPEED_Q4;
    int16_t target_strafe = (int16_t)strafe * TS_RUN_SPEED_Q4;
    int16_t dx_q4, dy_q4, fdx, fdy, sdx, sdy;
    int8_t sn, cs;
    uint8_t scale = s->speed_scale;

    s->speed_q4 = slew_i16(s->speed_q4, target_speed, TS_ACCEL_Q4);
    s->strafe_q4 = slew_i16(s->strafe_q4, target_strafe, TS_ACCEL_Q4);

    if (manual_turn) {
        int16_t desired = 0;
        if (manual_turn == 1u) desired = -TS_MANUAL_TURN_Q4;
        else if (manual_turn == 2u) desired = TS_MANUAL_TURN_Q4;
        s->turn_q4 = slew_i16(s->turn_q4, desired, TS_MANUAL_TURN_ACCEL_Q4);
    } else if (s->manual) {
        s->turn_q4 = slew_i16(s->turn_q4, 0, TS_MANUAL_TURN_ACCEL_Q4);
    } else {
        int16_t desired;
        int8_t e = yaw_error(target_yaw, s->yaw);
        desired = (int16_t)e << 2;
        if (desired > TS_AUTO_TURN_Q4) desired = TS_AUTO_TURN_Q4;
        if (desired < -TS_AUTO_TURN_Q4) desired = -TS_AUTO_TURN_Q4;
        s->turn_q4 = slew_i16(s->turn_q4, desired, TS_AUTO_TURN_ACCEL_Q4);
    }

    {
        int16_t yaw_step = s->turn_q4;
        if (!s->manual) yaw_step = scale_small(yaw_step, scale);
        if (yaw_step >= 0) s->yaw = (uint8_t)(s->yaw + (uint8_t)((yaw_step + 8) >> 4));
        else s->yaw = (uint8_t)(s->yaw - (uint8_t)(((-yaw_step) + 8) >> 4));
    }

    sn = k_sin[s->yaw];
    cs = k_sin[(uint8_t)(s->yaw + 64u)];
    fdx = (int16_t)(((int16_t)s->speed_q4 * cs) >> 11);
    fdy = (int16_t)(((int16_t)s->speed_q4 * sn) >> 11);
    sdx = (int16_t)((-(int16_t)s->strafe_q4 * sn) >> 11);
    sdy = (int16_t)(((int16_t)s->strafe_q4 * cs) >> 11);
    dx_q4 = scale_small((int16_t)(fdx + sdx), scale);
    dy_q4 = scale_small((int16_t)(fdy + sdy), scale);

    if (ts_is_walkable_q4((int16_t)(s->x_q4 + dx_q4), s->y_q4))
        s->x_q4 = (int16_t)(s->x_q4 + dx_q4);
    if (ts_is_walkable_q4(s->x_q4, (int16_t)(s->y_q4 + dy_q4)))
        s->y_q4 = (int16_t)(s->y_q4 + dy_q4);
}

void ts_step(TSState *s, uint8_t input) {
    uint8_t takeover = (uint8_t)(input & (TS_INPUT_UP | TS_INPUT_DOWN | TS_INPUT_LEFT |
                                          TS_INPUT_RIGHT | TS_INPUT_STRAFE_LEFT |
                                          TS_INPUT_STRAFE_RIGHT));
    int8_t throttle = 0;
    int8_t strafe = 0;
    uint8_t turn = 0u;

    if (input & TS_INPUT_SPEED) {
        ++s->speed_scale;
        if (s->speed_scale > 5u) s->speed_scale = 1u;
    }
    if (takeover) s->manual = 1u;

    if (s->manual) {
        if ((input & TS_INPUT_UP) && !(input & TS_INPUT_DOWN)) throttle = 1;
        else if ((input & TS_INPUT_DOWN) && !(input & TS_INPUT_UP)) throttle = -1;
        if ((input & TS_INPUT_STRAFE_LEFT) && !(input & TS_INPUT_STRAFE_RIGHT)) strafe = -1;
        else if ((input & TS_INPUT_STRAFE_RIGHT) && !(input & TS_INPUT_STRAFE_LEFT)) strafe = 1;
        if ((input & TS_INPUT_LEFT) && !(input & TS_INPUT_RIGHT)) turn = 1u;
        else if ((input & TS_INPUT_RIGHT) && !(input & TS_INPUT_LEFT)) turn = 2u;
        apply_motion(s, throttle, strafe, s->yaw, turn);
        return;
    }

    {
        const TSDemoPhase *p = &k_demo[s->demo_phase];
        apply_motion(s, p->throttle, 0, p->yaw, 0u);
        s->demo_ticks = (uint16_t)(s->demo_ticks + s->speed_scale);
        if (s->demo_ticks >= p->frames) {
            s->demo_ticks = 0u;
            if (s->demo_phase + 1u < TS_DEMO_PHASES) ++s->demo_phase;
            else s->demo_ticks = p->frames;
        }
    }
}

/* ----- Q4 camera/projection ----- */

static int16_t round_shift7(int16_t v) {
    if (v >= 0) return (int16_t)((v + 64) >> 7);
    return (int16_t)-(((-v) + 64) >> 7);
}

static void transform_vertices_q4(const TSState *s) {
    uint8_t vi;
    int16_t px_i = (int16_t)(s->x_q4 >> 4);
    int16_t py_i = (int16_t)(s->y_q4 >> 4);
    uint8_t fx = (uint8_t)(s->x_q4 & 15);
    uint8_t fy = (uint8_t)(s->y_q4 & 15);
    int8_t sn = k_sin[s->yaw];
    int8_t cs = k_sin[(uint8_t)(s->yaw + 64u)];
    int16_t frac_z = (int16_t)((int16_t)fx * cs + (int16_t)fy * sn);
    int16_t frac_x = (int16_t)(-(int16_t)fx * sn + (int16_t)fy * cs);
    int16_t frac_z_q4 = round_shift7(frac_z);
    int16_t frac_x_q4 = round_shift7(frac_x);

    for (vi = 0u; vi < TS_VERTICES; ++vi) {
        int16_t dx = (int16_t)k_vertices[vi].x - px_i;
        int16_t dy = (int16_t)k_vertices[vi].y - py_i;
        int16_t bz = (int16_t)((int16_t)dx * cs + (int16_t)dy * sn);
        int16_t bx = (int16_t)(-(int16_t)dx * sn + (int16_t)dy * cs);
        /* bz/8 is the rotated integer-world contribution expressed in Q4.
         * Subtract the rotated player fractional remainder instead of dropping it. */
        g_cam_z_q4[vi] = (int16_t)((bz >> 3) - frac_z_q4);
        g_cam_x_q4[vi] = (int16_t)((bx >> 3) - frac_x_q4);
    }
}

static uint8_t inv_for_zq4(int16_t z_q4) {
    uint8_t zi, frac, a, b;
    int16_t d;
    if (z_q4 <= TS_NEAR_Z_Q4) return k_invz[TS_NEAR_Z];
    if (z_q4 >= TS_FAR_Z_Q4) return k_invz[TS_FAR_Z];
    zi = (uint8_t)(z_q4 >> 4);
    frac = (uint8_t)(z_q4 & 15);
    a = k_invz[zi];
    b = k_invz[(uint8_t)(zi + 1u)];
    d = (int16_t)b - (int16_t)a;
    return (uint8_t)((int16_t)a + ((d * frac + (d >= 0 ? 8 : -8)) >> 4));
}

/* Exact positive quotient decomposition avoids a 32-bit multiply:
 * px ~= cam_x_q4 * inv / 512. */
static int16_t project_x_q4(int16_t cam_x_q4, int16_t cam_z_q4) {
    uint8_t inv;
    uint16_t ax, xi, xf, p, rem, extra;
    int16_t px;
    uint8_t neg = 0u;

    if (cam_x_q4 < 0) { neg = 1u; ax = (uint16_t)(-cam_x_q4); }
    else ax = (uint16_t)cam_x_q4;
    if (ax > (uint16_t)(127u << 4)) ax = (uint16_t)(127u << 4);

    inv = inv_for_zq4(cam_z_q4);
    xi = (uint16_t)(ax >> 4);
    xf = (uint16_t)(ax & 15u);
    p = (uint16_t)(xi * inv);          /* <= 32385 */
    px = (int16_t)(p >> 5);
    rem = (uint16_t)(p & 31u);
    extra = (uint16_t)(((rem << 4) + xf * inv) >> 9);
    px = (int16_t)(px + (int16_t)extra);
    if (neg) px = (int16_t)-px;
    return (int16_t)(80 + px);
}

static inline int8_t screen_col_floor(int16_t px) {
    if (px >= 0) return (int8_t)(px >> 3);
    return (int8_t)-(((-px) + 7) >> 3);
}

static inline int8_t row_floor(int16_t py) {
    if (py >= 0) return (int8_t)(py >> 3);
    return (int8_t)-(((-py) + 7) >> 3);
}

static inline uint8_t shade_for(uint8_t inv, int8_t bias) {
    int8_t shade;
    if (inv >= 82u) shade = 2;
    else if (inv >= 46u) shade = 1;
    else shade = 0;
    shade = (int8_t)(shade + bias);
    if (shade < 0) shade = 0;
    if (shade > 2) shade = 2;
    return (uint8_t)shade;
}

/* Q8 fraction a/b for 0<=a<b. Eight shift/subtract steps avoid the
 * compiler's 32-bit multiply/divide helpers; this path is rare anyway. */
static uint8_t ratio_q8(uint16_t a, uint16_t b) {
    uint8_t i, q = 0u;
    uint16_t r = a;
    if (b == 0u || a == 0u) return 0u;
    if (a >= b) return 255u;
    for (i = 0u; i < 8u; ++i) {
        r = (uint16_t)(r << 1);
        q = (uint8_t)(q << 1);
        if (r >= b) { r = (uint16_t)(r - b); q |= 1u; }
    }
    return q;
}

/* (signed v * unsigned q8) >> 8 using only 16-bit products. Split the
 * magnitude into whole 256s + an 8-bit remainder so neither product exceeds
 * 65535. */
static int16_t scale_q8_s16(int16_t v, uint8_t q8) {
    uint16_t a = (uint16_t)(v < 0 ? -v : v);
    uint16_t hi = (uint16_t)(a >> 8);
    uint16_t lo = (uint16_t)(a & 255u);
    uint16_t out = (uint16_t)(hi * q8 + ((lo * q8) >> 8));
    return v < 0 ? -(int16_t)out : (int16_t)out;
}

static int16_t clip_x_near_q4(int16_t x0, int16_t z0, int16_t x1, int16_t z1) {
    int16_t den = (int16_t)(z1 - z0);
    uint16_t a;
    uint8_t t_q8;
    if (den <= 0) return x0;
    a = (uint16_t)(TS_NEAR_Z_Q4 - z0);
    t_q8 = ratio_q8(a, (uint16_t)den);
    return (int16_t)(x0 + scale_q8_s16((int16_t)(x1 - x0), t_q8));
}

static uint8_t project_segment_span_uncached(uint8_t seg_id, TSProjectedSpan *p) {
    const TSSegment *seg = &k_segments[seg_id];
    int16_t x0 = g_cam_x_q4[seg->a], z0 = g_cam_z_q4[seg->a];
    int16_t x1 = g_cam_x_q4[seg->b], z1 = g_cam_z_q4[seg->b];
    int16_t sx0, sx1;
    uint8_t inv0, inv1, span;
    int8_t c0, c1;

    if (z0 < TS_NEAR_Z_Q4 && z1 < TS_NEAR_Z_Q4) return 0u;
    if (z0 < TS_NEAR_Z_Q4) { x0 = clip_x_near_q4(x0,z0,x1,z1); z0 = TS_NEAR_Z_Q4; }
    if (z1 < TS_NEAR_Z_Q4) { x1 = clip_x_near_q4(x1,z1,x0,z0); z1 = TS_NEAR_Z_Q4; }

    /* Cheap ~90-degree horizontal frustum reject before projection. */
    if (x0 < -z0 && x1 < -z1) return 0u;
    if (x0 >  z0 && x1 >  z1) return 0u;

    sx0 = project_x_q4(x0,z0);
    sx1 = project_x_q4(x1,z1);
    inv0 = inv_for_zq4(z0);
    inv1 = inv_for_zq4(z1);
    if (sx0 > sx1) {
        int16_t tx = sx0;
        uint8_t ti = inv0;
        sx0 = sx1; sx1 = tx;
        inv0 = inv1; inv1 = ti;
    }
    if (sx1 < 0 || sx0 > 159 || sx0 == sx1) return 0u;

    c0 = screen_col_floor(sx0);
    c1 = screen_col_floor(sx1);
    if (c1 < c0) return 0u;
    p->original_c0 = c0;
    p->original_c1 = c1;
    span = (uint8_t)(c1 - c0);
    if (span == 0u) span = 1u;
    if (span > 63u) span = 63u;
    p->inv_q6 = (int16_t)inv0 << 6;
    p->step_q6 = (int16_t)(((int16_t)inv1 - (int16_t)inv0) * (int16_t)k_span_recip_q6[span]);

    /* Do not iterate phantom offscreen columns. Advance the interpolation only
     * as far as necessary, then raster strictly within 0..19. */
    while (c0 < 0) { p->inv_q6 = (int16_t)(p->inv_q6 + p->step_q6); ++c0; }
    if (c1 >= (int8_t)TS_COLS) c1 = (int8_t)(TS_COLS - 1u);
    if (c0 >= (int8_t)TS_COLS || c1 < 0 || c1 < c0) return 0u;
    p->c0 = c0;
    p->c1 = c1;
    return 1u;
}

static const TSProjectedSpan *project_segment_span(uint8_t seg_id) {
    uint8_t state=g_span_state[seg_id];
    if (state==0u) {
        state=project_segment_span_uncached(seg_id,&g_span_cache[seg_id]) ? 2u : 1u;
        g_span_state[seg_id]=state;
    }
    return state==2u ? &g_span_cache[seg_id] : (const TSProjectedSpan *)0;
}

/* ----- tile edge/fill raster ----- */

static inline int16_t q6_round_px(int16_t v) {
    if (v >= 0) return (int16_t)((v + 32) >> 6);
    return (int16_t)-(((-v) + 32) >> 6);
}

static inline void profile_y_q6(uint8_t profile, int16_t inv_q6,
                         int16_t *top_q6, int16_t *bottom_q6,
                         uint8_t *snap_top, uint8_t *snap_bottom) {
    int16_t half = (int16_t)(inv_q6 >> 1);
    *top_q6 = (int16_t)((TS_HORIZON << 6) - half);
    *bottom_q6 = (int16_t)((TS_HORIZON << 6) + half);
    *snap_top = 0u;
    *snap_bottom = 0u;
    if (profile == TS_PROFILE_LINTEL) {
        *bottom_q6 = (int16_t)((TS_HORIZON << 6) - (half >> 1));
        *snap_bottom = 1u;
    } else if (profile == TS_PROFILE_RAISED_FULL) {
        *bottom_q6 = (int16_t)((TS_HORIZON << 6) + half - (half >> 2));
    } else if (profile == TS_PROFILE_RISER) {
        *top_q6 = (int16_t)((TS_HORIZON << 6) + half - (half >> 2));
        *snap_top = 1u;
    }
}

/* The pattern ROM stores only positive rises. A negative line is the same
 * pattern H-flipped with its starting phase shifted by -magnitude. Bottom edges
 * reuse the same geometry V-flipped plus palette 1 (outside -> floor). */
static inline uint16_t edge_entry(uint8_t shade, int16_t local_left, int8_t slope, uint8_t bottom) {
    uint16_t attr = 0u;
    uint8_t mag;
    int8_t off;

    if (bottom) {
        local_left = (int16_t)(7 - local_left);
        slope = (int8_t)-slope;
        attr = (uint16_t)(TS_ATTR_FLIPY | TS_ATTR_PALETTE);
    }

    if (slope < 0) {
        mag = (uint8_t)(-slope);
        local_left = (int16_t)(local_left - (int16_t)mag);
        attr = (uint16_t)(attr | TS_ATTR_FLIPX);
    } else mag = (uint8_t)slope;
    if (mag >= TS_EDGE_SLOPE_COUNT) mag = (uint8_t)(TS_EDGE_SLOPE_COUNT - 1u);

    off = clamp_s8(local_left, TS_EDGE_OFF_MIN,
                   (int8_t)(TS_EDGE_OFF_MIN + TS_EDGE_OFF_COUNT - 1));
    return (uint16_t)(TS_TILE_EDGE(shade,(uint8_t)(off-TS_EDGE_OFF_MIN),mag) | attr);
}

static inline void draw_full_rows(uint16_t out_map[TS_MAP_CELLS], uint8_t col,
                           int8_t first, int8_t last, uint8_t shade, uint8_t border,
                           uint8_t cap_first, uint8_t cap_last,
                           uint8_t clip_first, uint8_t clip_last) {
    int8_t r,plain_last;
    uint16_t idx,plain;
    if (first < (int8_t)clip_first) first = (int8_t)clip_first;
    if (last > (int8_t)clip_last) last = (int8_t)clip_last;
    if (first > last) return;
    idx=(uint16_t)(k_row_base[(uint8_t)first]+col);

    /* Interior rows are overwhelmingly the common case. Compute that tile ID
     * once, then make the hot loop one store + one constant stride. */
    plain=TS_TILE_FULL(shade,TS_CAP_NONE,border);
    if (first==last) {
        uint8_t cap=cap_last ? TS_CAP_BOTTOM : (cap_first ? TS_CAP_TOP : TS_CAP_NONE);
        out_map[idx]=cap==TS_CAP_NONE ? plain : TS_TILE_FULL(shade,cap,border);
        return;
    }
    if (cap_first) {
        out_map[idx]=TS_TILE_FULL(shade,TS_CAP_TOP,border);
        idx=(uint16_t)(idx+TS_COLS);
        ++first;
    }
    plain_last=cap_last ? (int8_t)(last-1) : last;
    for (r=first;r<=plain_last;++r) {
        out_map[idx]=plain;
        idx=(uint16_t)(idx+TS_COLS);
    }
    if (cap_last) out_map[idx]=TS_TILE_FULL(shade,TS_CAP_BOTTOM,border);
}

static inline void draw_edge_rows(uint16_t out_map[TS_MAP_CELLS], uint8_t col,
                           int16_t left_y, int16_t right_y, uint8_t shade, uint8_t bottom,
                           uint8_t clip_first, uint8_t clip_last) {
    int8_t slope=clamp_s8((int16_t)(right_y-left_y),-7,7);
    int8_t r0=row_floor(left_y<right_y?left_y:right_y);
    int8_t r1=row_floor(left_y>right_y?left_y:right_y);
    int8_t r;
    if (r0 < (int8_t)clip_first) r0=(int8_t)clip_first;
    if (r1 > (int8_t)clip_last) r1=(int8_t)clip_last;
    if (r0<0) r0=0;
    if (r1>=(int8_t)TS_ROWS) r1=(int8_t)(TS_ROWS-1u);
    for (r=r0;r<=r1;++r) {
        int16_t local=(int16_t)(left_y-((int16_t)r<<3));
        out_map[k_row_base[(uint8_t)r]+col]=edge_entry(shade,local,slope,bottom);
    }
}

/* Raster one already-visible surface column. The caller supplies connected
 * edge endpoints; this function only writes the small set of tile rows that the
 * surface actually occupies. No depth compare and no row*20 multiply remain. */
static inline void raster_surface_column(uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS],
                                  uint8_t col, uint8_t seg_id,
                                  int16_t inv_l_q6, int16_t inv_r_q6,
                                  int16_t top_l, int16_t top_r,
                                  int16_t bot_l, int16_t bot_r,
                                  uint8_t snap_top, uint8_t snap_bottom,
                                  uint8_t border,
                                  uint8_t *clip_top, uint8_t *clip_bottom,
                                  uint8_t mutate_clip) {
    const TSSegment *seg = &k_segments[seg_id];
    uint8_t shade = shade_for((uint8_t)(((inv_l_q6 + inv_r_q6) >> 1) >> 6),seg->shade_bias);
    uint8_t clip_first, clip_last;
    int8_t top_min_row, top_max_row, bot_min_row, bot_max_row;
    int16_t top_min = top_l < top_r ? top_l : top_r;
    int16_t top_max = top_l > top_r ? top_l : top_r;
    int16_t bot_min = bot_l < bot_r ? bot_l : bot_r;
    int16_t bot_max = bot_l > bot_r ? bot_l : bot_r;

    if (*clip_top > *clip_bottom) return;
    clip_first = (uint8_t)((*clip_top + 7u) >> 3);
    clip_last = (uint8_t)(*clip_bottom >> 3);
    if (clip_first >= TS_ROWS || clip_last >= TS_ROWS || clip_first > clip_last) return;

    top_min_row = row_floor(top_min);
    top_max_row = row_floor(top_max);
    bot_min_row = row_floor(bot_min);
    bot_max_row = row_floor(bot_max);

    if (snap_top) {
        draw_full_rows(out_map,col,top_min_row,top_min_row,shade,border,1u,0u,clip_first,clip_last);
    } else {
        draw_edge_rows(out_map,col,top_l,top_r,shade,0u,clip_first,clip_last);
    }

    if (snap_bottom) {
        draw_full_rows(out_map,col,bot_max_row,bot_max_row,shade,border,0u,1u,clip_first,clip_last);
    } else {
        draw_edge_rows(out_map,col,bot_l,bot_r,shade,1u,clip_first,clip_last);
    }

    draw_full_rows(out_map,col,(int8_t)(top_max_row+1),(int8_t)(bot_min_row-1),shade,border,0u,0u,
                   clip_first,clip_last);

    /* Host tests expose nearest-wall diagnostics, but the Game Gear display never
     * consumes TSColumn. Do not pay these 16-bit comparisons/stores on SDCC. */
#ifndef __SDCC
    {
        uint8_t inv = (uint8_t)(((inv_l_q6 + inv_r_q6) >> 1) >> 6);
        if (inv > cols[col].invz) {
            cols[col].invz = inv;
            cols[col].wall_id = seg_id;
            cols[col].shade = shade;
            cols[col].top = clamp_u8(top_l,143u);
            cols[col].bottom = clamp_u8(bot_l,143u);
            cols[col].top_step = clamp_s8((int16_t)(top_r-top_l),-7,7);
            cols[col].bottom_step = clamp_s8((int16_t)(bot_r-bot_l),-7,7);
        }
    }
#else
    (void)cols;
#endif

    if (!mutate_clip) return;
    if (seg->profile == TS_PROFILE_FULL || seg->profile == TS_PROFILE_RAISED_FULL) {
        *clip_top = 1u;
        *clip_bottom = 0u; /* ray closed: farther sectors cannot matter here */
    } else if (seg->profile == TS_PROFILE_LINTEL) {
        /* Stage 4 will selectively composite within a shared 8x8 cell. Until
         * then, advance to the next whole tile row so far geometry never
         * overwrites the near lintel boundary tile. */
        int16_t y = bot_max;
        int16_t next = (int16_t)(((y >> 3) + 1) << 3);
        if (next > *clip_top) *clip_top = clamp_u8(next,143u);
    } else if (seg->profile == TS_PROFILE_RISER) {
        int16_t y = top_min;
        int16_t prev = (int16_t)(((y >> 3) << 3) - 1);
        if (prev < (int16_t)*clip_bottom) *clip_bottom = clamp_u8(prev,143u);
    }
}

/* Quantize one tile's ideal right endpoint while forcing its left endpoint to
 * equal the previous tile's actual endpoint. The residual geometric error is
 * therefore carried into the next tile automatically: tile-scale Bresenham. */
static inline int16_t connected_end(int16_t actual_left, int16_t ideal_right) {
    int16_t d = (int16_t)(ideal_right - actual_left);
    if (d > 7) d = 7;
    if (d < -7) d = -7;
    return (int16_t)(actual_left + d);
}

static void candidate_reset(uint8_t view_c0,uint8_t view_c1) {
    uint8_t c;
    for (c=view_c0;c<=view_c1;++c) g_best_seg[c]=TS_NO_WALL;
}

static void candidate_add_segment(uint8_t seg_id,uint8_t view_c0,uint8_t view_c1) {
    const TSProjectedSpan *p=project_segment_span(seg_id);
    int8_t c,c0,c1;
    int16_t inv_q6;
    if (!p) return;
    c0=p->c0;
    c1=p->c1;
    inv_q6=p->inv_q6;
    while (c0<(int8_t)view_c0) { inv_q6=(int16_t)(inv_q6+p->step_q6); ++c0; }
    if (c1>(int8_t)view_c1) c1=(int8_t)view_c1;
    if (c0>c1) return;
    for (c=c0;c<=c1;++c) {
        uint8_t uc=(uint8_t)c;
        int16_t next_q6=(int16_t)(inv_q6+p->step_q6);
        uint8_t inv=(uint8_t)(((inv_q6+next_q6)>>1)>>6);
        if (g_best_seg[uc]==TS_NO_WALL || inv>g_best_inv[uc]) {
            uint8_t border=0u;
            if (c==p->original_c0 && p->original_c0>=0) border|=1u;
            if (c==p->original_c1 && p->original_c1<(int8_t)TS_COLS) border|=2u;
            g_best_seg[uc]=seg_id;
            g_best_inv[uc]=inv;
            g_best_border[uc]=border;
            g_best_inv_l_q6[uc]=inv_q6;
            g_best_inv_r_q6[uc]=next_q6;
        }
        inv_q6=next_q6;
    }
}

static void build_sector_candidates(uint8_t sector,uint8_t view_c0,uint8_t view_c1) {
    uint8_t i;
    candidate_reset(view_c0,view_c1);
    for (i=0u;i<k_sector_count[sector];++i)
        candidate_add_segment(k_sector_segments[sector][i],view_c0,view_c1);
}

static void render_sector_candidates(uint8_t depth,uint8_t view_c0,uint8_t view_c1,
                                     uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint8_t c;
    uint8_t prev_seg=TS_NO_WALL;
    int16_t carry_top=0,carry_bottom=0;

    for (c=view_c0;c<=view_c1;++c) {
        uint8_t seg_id=g_best_seg[c];
        int16_t tlq,trq,blq,brq;
        int16_t top_l,top_target,top_r,bot_l,bot_target,bot_r;
        uint8_t stl,sbl,str,sbr;
        if (seg_id==TS_NO_WALL || g_clip_top[depth][c]>g_clip_bottom[depth][c]) {
            prev_seg=TS_NO_WALL;
            continue;
        }
        profile_y_q6(k_segments[seg_id].profile,g_best_inv_l_q6[c],&tlq,&blq,&stl,&sbl);
        profile_y_q6(k_segments[seg_id].profile,g_best_inv_r_q6[c],&trq,&brq,&str,&sbr);
        if (stl||str) {
            top_l=(int16_t)((q6_round_px(tlq)+4)&~7);
            top_target=(int16_t)((q6_round_px(trq)+4)&~7);
        } else { top_l=q6_round_px(tlq); top_target=q6_round_px(trq); }
        if (sbl||sbr) {
            bot_l=(int16_t)(((q6_round_px(blq)+4)&~7)-1);
            bot_target=(int16_t)(((q6_round_px(brq)+4)&~7)-1);
        } else { bot_l=q6_round_px(blq); bot_target=q6_round_px(brq); }
        if (prev_seg==seg_id) { top_l=carry_top; bot_l=carry_bottom; }
        top_r=connected_end(top_l,top_target);
        bot_r=connected_end(bot_l,bot_target);
        carry_top=top_r; carry_bottom=bot_r; prev_seg=seg_id;
        raster_surface_column(out_map,cols,c,seg_id,
                              g_best_inv_l_q6[c],g_best_inv_r_q6[c],
                              top_l,top_r,bot_l,bot_r,
                              (uint8_t)(stl||str),(uint8_t)(sbl||sbr),g_best_border[c],
                              &g_clip_top[depth][c],&g_clip_bottom[depth][c],1u);
    }
}

static uint8_t portal_other_sector(uint8_t portal,uint8_t sector) {
    return k_portals[portal].sector_a==sector ? k_portals[portal].sector_b : k_portals[portal].sector_a;
}

static void raster_portal_face(uint8_t seg_id,const TSProjectedSpan *p,uint8_t depth,
                               uint8_t view_c0,uint8_t view_c1,
                               uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]) {
    int8_t c,c0=p->c0,c1=p->c1;
    int16_t inv_q6=p->inv_q6;
    int16_t carry_top=0,carry_bottom=0;
    uint8_t have_carry=0u;
    if (seg_id==TS_NO_WALL) return;
    while (c0<(int8_t)view_c0) { inv_q6=(int16_t)(inv_q6+p->step_q6); ++c0; }
    if (c1>(int8_t)view_c1) c1=(int8_t)view_c1;
    for (c=c0;c<=c1;++c) {
        int16_t next_q6=(int16_t)(inv_q6+p->step_q6);
        int16_t tlq,trq,blq,brq;
        int16_t top_l,top_target,top_r,bot_l,bot_target,bot_r;
        uint8_t stl,sbl,str,sbr,border=0u;
        uint8_t uc=(uint8_t)c;
        if (g_clip_top[depth][uc]<=g_clip_bottom[depth][uc]) {
            profile_y_q6(k_segments[seg_id].profile,inv_q6,&tlq,&blq,&stl,&sbl);
            profile_y_q6(k_segments[seg_id].profile,next_q6,&trq,&brq,&str,&sbr);
            if (stl||str) {
                top_l=(int16_t)((q6_round_px(tlq)+4)&~7);
                top_target=(int16_t)((q6_round_px(trq)+4)&~7);
            } else { top_l=q6_round_px(tlq); top_target=q6_round_px(trq); }
            if (sbl||sbr) {
                bot_l=(int16_t)(((q6_round_px(blq)+4)&~7)-1);
                bot_target=(int16_t)(((q6_round_px(brq)+4)&~7)-1);
            } else { bot_l=q6_round_px(blq); bot_target=q6_round_px(brq); }
            if (have_carry) { top_l=carry_top; bot_l=carry_bottom; }
            top_r=connected_end(top_l,top_target);
            bot_r=connected_end(bot_l,bot_target);
            carry_top=top_r; carry_bottom=bot_r; have_carry=1u;
            if (c==p->original_c0&&p->original_c0>=0) border|=1u;
            if (c==p->original_c1&&p->original_c1<(int8_t)TS_COLS) border|=2u;
            raster_surface_column(out_map,cols,uc,seg_id,inv_q6,next_q6,
                                  top_l,top_r,bot_l,bot_r,
                                  (uint8_t)(stl||str),(uint8_t)(sbl||sbr),border,
                                  &g_clip_top[depth][uc],&g_clip_bottom[depth][uc],1u);
        } else have_carry=0u;
        inv_q6=next_q6;
    }
}

static void render_sector(uint8_t sector,uint8_t parent_portal,uint8_t depth,
                          uint8_t view_c0,uint8_t view_c1,
                          uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]);

static void render_portal(uint8_t portal,uint8_t sector,uint8_t depth,
                          uint8_t view_c0,uint8_t view_c1,
                          uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]) {
    const TSProjectedSpan *p=project_segment_span(k_portals[portal].lintel_seg);
    uint8_t child_depth=(uint8_t)(depth+1u),child_sector,c,child_c0,child_c1;
    uint8_t found=0u;
    if (child_depth>=TS_MAX_PORTAL_DEPTH || !p) return;
    child_c0=(uint8_t)(p->c0>(int8_t)view_c0?p->c0:(int8_t)view_c0);
    child_c1=(uint8_t)(p->c1<(int8_t)view_c1?p->c1:(int8_t)view_c1);
    if (child_c0>child_c1) return;

    /* Initialize only the horizontal interval that the child traversal will
     * actually inspect. Closed parent columns are copied as closed; no 20-column reset. */
    for (c=child_c0;c<=child_c1;++c) {
        g_clip_top[child_depth][c]=g_clip_top[depth][c];
        g_clip_bottom[child_depth][c]=g_clip_bottom[depth][c];
        if (g_clip_top[child_depth][c]<=g_clip_bottom[child_depth][c]) found=1u;
    }
    if (!found) return;

    /* Lintel and riser share portal endpoints, so the one cached projected span
     * drives both faces instead of projecting the opening two or three times. */
    g_ts_render_stage=6u;
    raster_portal_face(k_portals[portal].lintel_seg,p,child_depth,child_c0,child_c1,out_map,cols);
    if (k_portals[portal].riser_seg!=TS_NO_WALL)
        raster_portal_face(k_portals[portal].riser_seg,p,child_depth,child_c0,child_c1,out_map,cols);

    g_ts_render_stage=5u;
    child_sector=portal_other_sector(portal,sector);
    render_sector(child_sector,portal,child_depth,child_c0,child_c1,out_map,cols);
}

static void render_sector(uint8_t sector,uint8_t parent_portal,uint8_t depth,
                          uint8_t view_c0,uint8_t view_c1,
                          uint16_t out_map[TS_MAP_CELLS],TSColumn cols[TS_COLS]) {
    uint8_t i,portal;
    g_ts_render_stage=3u;
    build_sector_candidates(sector,view_c0,view_c1);
    g_ts_render_stage=4u;
    render_sector_candidates(depth,view_c0,view_c1,out_map,cols);
    g_ts_render_stage=5u;
    for (i=0u;i<k_sector_portal_count[sector];++i) {
        portal=k_sector_portals[sector][i];
        if (portal==parent_portal) continue;
        render_portal(portal,sector,depth,view_c0,view_c1,out_map,cols);
        g_ts_render_stage=5u;
    }
}

static uint8_t current_sector(const TSState *s) {
    int16_t x=s->x_q4;
    if (x < (int16_t)(80 << 4)) return 0u;
    if (x < (int16_t)(112 << 4)) return 1u;
    return 2u;
}

static void clear_frame(uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint8_t c,y;
    if(!g_base_map_ready) {
        uint16_t *dst=g_base_map;
        for(y=0u;y<TS_ROWS;++y) {
            uint16_t base=(y<9u)?TS_TILE_CEILING:(y==9u?TS_TILE_HORIZON:TS_TILE_FLOOR);
            for(c=0u;c<TS_COLS;++c) *dst++=base;
        }
        g_base_map_ready=1u;
    }
    /* GBDK's memcpy maps to a tight block copy; this removes hundreds of
     * per-frame C loop/index operations from the clear stage. */
    memcpy(out_map,g_base_map,sizeof(g_base_map));
    for(c=0u;c<TS_COLS;++c) {
        cols[c].invz=0u;
        cols[c].wall_id=TS_NO_WALL;
        cols[c].shade=0u;
        cols[c].top=TS_HORIZON;
        cols[c].bottom=TS_HORIZON;
        cols[c].top_step=0;
        cols[c].bottom_step=0;
        g_clip_top[0][c]=0u;
        g_clip_bottom[0][c]=143u;
    }
}

void ts_render_columns(const TSState *s, TSColumn cols[TS_COLS]) {
    uint16_t dummy[TS_MAP_CELLS];
    ts_build_tilemap(s,dummy,cols);
}

void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint8_t sector;
    g_ts_render_stage=1u;
    clear_frame(out_map,cols);

    g_ts_render_stage=2u;
    transform_vertices_q4(s);
    memset(g_span_state,0,sizeof(g_span_state));

    g_ts_render_stage=3u;
    sector=current_sector(s);
    render_sector(sector,TS_NO_PORTAL,0u,0u,(uint8_t)(TS_COLS-1u),out_map,cols);

    g_ts_render_stage=0u;
}
