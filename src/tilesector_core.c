#include "tilesector_core.h"

#define TS_NEAR_Z 10
#define TS_FAR_Z 127
#define TS_HORIZON 72
#define TS_RUN_SPEED_Q4 192
#define TS_ACCEL_Q4 6
#define TS_MANUAL_TURN_Q4 48
#define TS_MANUAL_TURN_ACCEL_Q4 16
#define TS_AUTO_TURN_Q4 40
#define TS_AUTO_TURN_ACCEL_Q4 4
#define TS_NO_WALL 0xffu

typedef struct { int16_t x, y; } TSVertex;
typedef struct { uint8_t a, b; int8_t shade_bias; } TSSegment;
typedef struct { uint16_t frames; uint8_t yaw; int8_t throttle; } TSDemoPhase;

static const TSVertex k_vertices[] = {
    { 16, 16 }, { 80, 16 }, { 80, 36 }, { 80, 64 }, { 80, 80 }, { 16, 80 },
    {112, 36 }, {112, 64 }, {112, 14 }, {112, 84 },
    {144, 10 }, {160, 18 }, {176, 14 }, {176, 84 }
};

static const TSSegment k_segments[] = {
    {0,1, 0}, {1,2, 0}, {3,4, 0}, {4,5, 0}, {5,0, 0},
    {2,6,-1}, {7,3,-1}, {8,6, 0}, {7,9, 0},
    {8,10,1}, {10,11,0}, {11,12,1}, {12,13,0}, {13,9,0}
};

static const TSDemoPhase k_demo[] = {
    {128u,   0u,  1 },
    { 18u,  28u,  1 },
    { 18u,  54u,  1 },
    { 18u,  62u,  1 },
    { 18u,  38u,  1 },
    { 18u,  16u,  1 },
    { 20u,   0u,  1 },
    { 64u,   0u,  0 }
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

static int16_t abs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

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

uint8_t ts_is_walkable_q8(int16_t x_q8, int16_t y_q8) {
    int16_t x = (int16_t)(x_q8 >> 4);
    int16_t y = (int16_t)(y_q8 >> 4);
    if (x >= 20 && x <= 76 && y >= 20 && y <= 76) return 1u;
    if (x >= 74 && x <= 116 && y >= 40 && y <= 60) return 1u;
    if (x >= 112 && x <= 172 && y >= 20 && y <= 78) return 1u;
    return 0u;
}

void ts_reset(TSState *s) {
    s->x_q8 = (int16_t)(32 << 4);
    s->y_q8 = (int16_t)(48 << 4);
    s->yaw = 0u;
    s->speed_q4 = 0;
    s->turn_q4 = 0;
    s->manual = 0u;
    s->demo_phase = 0u;
    s->demo_ticks = 0u;
}

static void apply_motion(TSState *s, int8_t throttle, uint8_t target_yaw, uint8_t manual_turn) {
    int16_t target_speed = (int16_t)throttle * TS_RUN_SPEED_Q4;
    int16_t dx_q8, dy_q8;
    int8_t sn, cs;

    s->speed_q4 = slew_i16(s->speed_q4, target_speed, TS_ACCEL_Q4);

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
        desired = (int16_t)e * 4;
        if (desired > TS_AUTO_TURN_Q4) desired = TS_AUTO_TURN_Q4;
        if (desired < -TS_AUTO_TURN_Q4) desired = -TS_AUTO_TURN_Q4;
        s->turn_q4 = slew_i16(s->turn_q4, desired, TS_AUTO_TURN_ACCEL_Q4);
    }

    if (s->turn_q4 >= 0) s->yaw = (uint8_t)(s->yaw + (uint8_t)((s->turn_q4 + 8) >> 4));
    else s->yaw = (uint8_t)(s->yaw - (uint8_t)(((-s->turn_q4) + 8) >> 4));

    sn = k_sin[s->yaw];
    cs = k_sin[(uint8_t)(s->yaw + 64u)];
    dx_q8 = (int16_t)(((int16_t)s->speed_q4 * cs) >> 11);
    dy_q8 = (int16_t)(((int16_t)s->speed_q4 * sn) >> 11);

    if (ts_is_walkable_q8((int16_t)(s->x_q8 + dx_q8), s->y_q8)) s->x_q8 = (int16_t)(s->x_q8 + dx_q8);
    if (ts_is_walkable_q8(s->x_q8, (int16_t)(s->y_q8 + dy_q8))) s->y_q8 = (int16_t)(s->y_q8 + dy_q8);
}

void ts_step(TSState *s, uint8_t input) {
    uint8_t dirs = (uint8_t)(input & (TS_INPUT_UP | TS_INPUT_DOWN | TS_INPUT_LEFT | TS_INPUT_RIGHT));
    int8_t throttle = 0;
    uint8_t turn = 0u;

    if (input & TS_INPUT_START) {
        ts_reset(s);
        return;
    }
    if (dirs) s->manual = 1u;

    if (s->manual) {
        if ((input & TS_INPUT_UP) && !(input & TS_INPUT_DOWN)) throttle = 1;
        else if ((input & TS_INPUT_DOWN) && !(input & TS_INPUT_UP)) throttle = -1;
        if ((input & TS_INPUT_LEFT) && !(input & TS_INPUT_RIGHT)) turn = 1u;
        else if ((input & TS_INPUT_RIGHT) && !(input & TS_INPUT_LEFT)) turn = 2u;
        apply_motion(s, throttle, s->yaw, turn);
        return;
    }

    {
        const TSDemoPhase *p = &k_demo[s->demo_phase];
        apply_motion(s, p->throttle, p->yaw, 0u);
        ++s->demo_ticks;
        if (s->demo_ticks >= p->frames) {
            s->demo_ticks = 0u;
            if (s->demo_phase + 1u < TS_DEMO_PHASES) ++s->demo_phase;
            else s->demo_ticks = p->frames;
        }
    }
}

static int16_t project_x(int16_t cam_x, int16_t cam_z) {
    uint8_t z;
    int16_t px;
    if (cam_z < TS_NEAR_Z) cam_z = TS_NEAR_Z;
    if (cam_z > TS_FAR_Z) cam_z = TS_FAR_Z;
    if (cam_x < -127) cam_x = -127;
    if (cam_x > 127) cam_x = 127;
    z = (uint8_t)cam_z;
    px = (int16_t)(((int16_t)cam_x * (int16_t)k_invz[z]) >> 5);
    return (int16_t)(80 + px);
}

static int8_t screen_col_floor(int16_t px) {
    if (px >= 0) return (int8_t)(px >> 3);
    return (int8_t)-(((-px) + 7) >> 3);
}

static uint8_t shade_for(uint8_t inv, int8_t bias) {
    int8_t shade;
    if (inv >= 82u) shade = 2;
    else if (inv >= 46u) shade = 1;
    else shade = 0;
    shade = (int8_t)(shade + bias);
    if (shade < 0) shade = 0;
    if (shade > 2) shade = 2;
    return (uint8_t)shade;
}

void ts_render_columns(const TSState *s, TSColumn cols[TS_COLS]) {
    int16_t cam_x[sizeof(k_vertices) / sizeof(k_vertices[0])];
    int16_t cam_z[sizeof(k_vertices) / sizeof(k_vertices[0])];
    uint8_t vi, si, c;
    int16_t px_q4 = (int16_t)(s->x_q8 >> 4);
    int16_t py_q4 = (int16_t)(s->y_q8 >> 4);
    int8_t sn = k_sin[s->yaw];
    int8_t cs = k_sin[(uint8_t)(s->yaw + 64u)];

    for (c = 0u; c < TS_COLS; ++c) {
        cols[c].invz = 0u;
        cols[c].wall_id = TS_NO_WALL;
        cols[c].shade = 0u;
        cols[c].top = TS_HORIZON;
        cols[c].bottom = TS_HORIZON;
    }

    for (vi = 0u; vi < (uint8_t)(sizeof(k_vertices) / sizeof(k_vertices[0])); ++vi) {
        int16_t dx = (int16_t)k_vertices[vi].x - px_q4;
        int16_t dy = (int16_t)k_vertices[vi].y - py_q4;
        cam_z[vi] = (int16_t)(((int16_t)dx * cs + (int16_t)dy * sn) >> 7);
        cam_x[vi] = (int16_t)((-(int16_t)dx * sn + (int16_t)dy * cs) >> 7);
    }

    for (si = 0u; si < (uint8_t)(sizeof(k_segments) / sizeof(k_segments[0])); ++si) {
        const TSSegment *seg = &k_segments[si];
        int16_t x0 = cam_x[seg->a], z0 = cam_z[seg->a];
        int16_t x1 = cam_x[seg->b], z1 = cam_z[seg->b];
        int16_t sx0, sx1;
        uint8_t inv0, inv1;
        int8_t c0, c1, cc;
        uint8_t span;
        int16_t inv_q6, step_q6;

        if (z0 < TS_NEAR_Z && z1 < TS_NEAR_Z) continue;

        if (z0 < TS_NEAR_Z) {
            int16_t den = (int16_t)(z1 - z0);
            if (!den) continue;
            {
                int16_t t = (int16_t)((((int32_t)(TS_NEAR_Z - z0)) << 8) / den);
                x0 = (int16_t)(x0 + (int16_t)(((int32_t)(x1 - x0) * t) >> 8));
                z0 = TS_NEAR_Z;
            }
        }
        if (z1 < TS_NEAR_Z) {
            int16_t den = (int16_t)(z0 - z1);
            if (!den) continue;
            {
                int16_t t = (int16_t)((((int32_t)(TS_NEAR_Z - z1)) << 8) / den);
                x1 = (int16_t)(x1 + (int16_t)(((int32_t)(x0 - x1) * t) >> 8));
                z1 = TS_NEAR_Z;
            }
        }

        sx0 = project_x(x0, z0);
        sx1 = project_x(x1, z1);
        inv0 = k_invz[(uint8_t)(z0 > TS_FAR_Z ? TS_FAR_Z : z0)];
        inv1 = k_invz[(uint8_t)(z1 > TS_FAR_Z ? TS_FAR_Z : z1)];

        if (sx0 > sx1) {
            int16_t tx = sx0;
            uint8_t ti = inv0;
            sx0 = sx1; sx1 = tx; inv0 = inv1; inv1 = ti;
        }
        if (sx1 < 0 || sx0 > 159 || sx0 == sx1) continue;

        c0 = screen_col_floor(sx0);
        c1 = screen_col_floor(sx1);
        if (c0 < -20) c0 = -20;
        if (c1 > 39) c1 = 39;
        if (c1 < c0) continue;
        span = (uint8_t)(c1 - c0);
        if (span == 0u) span = 1u;
        if (span > 63u) span = 63u;
        inv_q6 = (int16_t)inv0 << 6;
        step_q6 = (int16_t)(((int16_t)inv1 - (int16_t)inv0) * (int16_t)k_span_recip_q6[span]);

        for (cc = c0; cc <= c1; ++cc) {
            if (cc >= 0 && cc < (int8_t)TS_COLS) {
                uint8_t inv = clamp_u8((int16_t)(inv_q6 >> 6), 255u);
                if (inv > cols[(uint8_t)cc].invz) {
                    uint8_t half = (uint8_t)(inv >> 1);
                    int16_t top = (int16_t)TS_HORIZON - half;
                    int16_t bot = (int16_t)TS_HORIZON + half;
                    cols[(uint8_t)cc].invz = inv;
                    cols[(uint8_t)cc].wall_id = si;
                    cols[(uint8_t)cc].shade = shade_for(inv, seg->shade_bias);
                    cols[(uint8_t)cc].top = clamp_u8(top, 143u);
                    cols[(uint8_t)cc].bottom = clamp_u8(bot, 143u);
                }
            }
            inv_q6 = (int16_t)(inv_q6 + step_q6);
        }
    }
}

static uint8_t border_flags(const TSColumn cols[TS_COLS], uint8_t c) {
    uint8_t b = 0u;
    if (cols[c].wall_id == TS_NO_WALL) return 0u;
    if (c == 0u || cols[c - 1u].wall_id != cols[c].wall_id || abs16((int16_t)cols[c].invz - cols[c - 1u].invz) > 18) b |= 1u;
    if (c == TS_COLS - 1u || cols[c + 1u].wall_id != cols[c].wall_id || abs16((int16_t)cols[c].invz - cols[c + 1u].invz) > 18) b |= 2u;
    return b;
}

void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint8_t x, y;
    ts_render_columns(s, cols);

    for (y = 0u; y < TS_ROWS; ++y) {
        uint16_t base = (y < 9u) ? TS_TILE_CEILING : (y == 9u ? TS_TILE_HORIZON : TS_TILE_FLOOR);
        for (x = 0u; x < TS_COLS; ++x) out_map[(uint16_t)y * TS_COLS + x] = base;
    }

    for (x = 0u; x < TS_COLS; ++x) {
        const TSColumn *col = &cols[x];
        uint8_t top_row, bot_row, b;
        if (col->wall_id == TS_NO_WALL || col->invz == 0u) continue;
        top_row = (uint8_t)(col->top >> 3);
        bot_row = (uint8_t)(col->bottom >> 3);
        b = border_flags(cols, x);

        if (top_row == bot_row) {
            out_map[(uint16_t)top_row * TS_COLS + x] = TS_TILE_FULL(col->shade, b);
            continue;
        }

        out_map[(uint16_t)top_row * TS_COLS + x] = TS_TILE_TOP(col->shade, (uint8_t)(col->top & 7u), b);
        for (y = (uint8_t)(top_row + 1u); y < bot_row; ++y)
            out_map[(uint16_t)y * TS_COLS + x] = TS_TILE_FULL(col->shade, b);
        out_map[(uint16_t)bot_row * TS_COLS + x] = TS_TILE_BOTTOM(col->shade, (uint8_t)(col->bottom & 7u), b);
    }
}
