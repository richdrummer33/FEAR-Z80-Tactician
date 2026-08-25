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
#define TS_SEGMENTS 17u

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

/* Two rooms, a short connector, two open arch planes, and a deliberately
 * obvious zig-zag north wall in room B. The arch planes are render-only:
 * the walkable topology stays open. */
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

/* Recorded player path. The corner is split into several desired headings so
 * turn velocity eases into and out of it. The second room arc deliberately
 * looks across the zig-zag wall before returning to the new forward tangent. */
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

/* 2560/z-ish reciprocal/projection scale. Runtime perspective is table lookup. */
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

/* Coarse 20x18 tile-cell Z field: enough to let a near lintel occupy upper
 * cells while a farther wall remains visible through the opening below it. */
static uint8_t g_depth[TS_MAP_CELLS];

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

static int8_t row_floor(int16_t py) {
    if (py >= 0) return (int8_t)(py >> 3);
    return (int8_t)-(((-py) + 7) >> 3);
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

/* Near-plane interpolation intentionally remains 16-bit. */
static int16_t clip_x_q7(int16_t x_near, int16_t z_near, int16_t x_far, int16_t z_far) {
    int16_t den = (int16_t)(z_far - z_near);
    int16_t t_q7;
    int16_t dx;
    if (den <= 0) return x_near;
    t_q7 = (int16_t)(((TS_NEAR_Z - z_near) << 7) / den);
    if (t_q7 < 0) t_q7 = 0;
    if (t_q7 > 128) t_q7 = 128;
    dx = (int16_t)(x_far - x_near);
    return (int16_t)(x_near + ((dx * t_q7) >> 7));
}

static void vertical_profile(uint8_t profile, uint8_t inv,
                             int16_t *top, int16_t *bottom,
                             uint8_t *snap_top, uint8_t *snap_bottom) {
    int16_t half = (int16_t)(inv >> 1);
    *top = (int16_t)TS_HORIZON - half;
    *bottom = (int16_t)TS_HORIZON + half;
    *snap_top = 0u;
    *snap_bottom = 0u;
    if (profile == TS_PROFILE_LINTEL) {
        *bottom = (int16_t)TS_HORIZON - (half >> 1);
        *snap_bottom = 1u;
    } else if (profile == TS_PROFILE_RAISED_FULL) {
        *bottom = (int16_t)TS_HORIZON + half - (half >> 2);
    } else if (profile == TS_PROFILE_RISER) {
        *top = (int16_t)TS_HORIZON + half - (half >> 2);
        *snap_top = 1u;
    }
}

static uint16_t edge_entry(uint8_t shade, int16_t local_left, int8_t slope, uint8_t bottom) {
    int8_t off = clamp_s8(local_left, TS_EDGE_OFF_MIN,
                           (int8_t)(TS_EDGE_OFF_MIN + TS_EDGE_OFF_COUNT - 1));
    int8_t sl = clamp_s8(slope, TS_EDGE_SLOPE_MIN,
                          (int8_t)(TS_EDGE_SLOPE_MIN + TS_EDGE_SLOPE_COUNT - 1));
    uint8_t oi, si;
    uint16_t entry;

    if (!bottom) {
        oi = (uint8_t)(off - TS_EDGE_OFF_MIN);
        si = (uint8_t)(sl - TS_EDGE_SLOPE_MIN);
        return TS_TILE_EDGE(shade, oi, si);
    }

    /* One edge family does both boundaries: V flip mirrors the vector line;
     * palette 1 changes only its outside colour from ceiling to floor. */
    off = (int8_t)(7 - off);
    sl = (int8_t)-sl;
    off = clamp_s8(off, TS_EDGE_OFF_MIN,
                   (int8_t)(TS_EDGE_OFF_MIN + TS_EDGE_OFF_COUNT - 1));
    sl = clamp_s8(sl, TS_EDGE_SLOPE_MIN,
                  (int8_t)(TS_EDGE_SLOPE_MIN + TS_EDGE_SLOPE_COUNT - 1));
    oi = (uint8_t)(off - TS_EDGE_OFF_MIN);
    si = (uint8_t)(sl - TS_EDGE_SLOPE_MIN);
    entry = TS_TILE_EDGE(shade, oi, si);
    return (uint16_t)(entry | TS_ATTR_FLIPY | TS_ATTR_PALETTE);
}

static void plot_cell(uint16_t out_map[TS_MAP_CELLS], uint8_t col, int8_t row,
                      uint8_t inv, uint16_t entry) {
    uint16_t idx;
    if (row < 0 || row >= (int8_t)TS_ROWS || col >= TS_COLS) return;
    idx = (uint16_t)(uint8_t)row * TS_COLS + col;
    if (inv > g_depth[idx]) {
        g_depth[idx] = inv;
        out_map[idx] = entry;
    }
}

static void draw_full_span(uint16_t out_map[TS_MAP_CELLS], uint8_t col,
                           int8_t first, int8_t last, uint8_t inv,
                           uint8_t shade, uint8_t border,
                           uint8_t first_cap, uint8_t last_cap) {
    int8_t r;
    if (first > last) return;
    for (r = first; r <= last; ++r) {
        uint8_t cap = TS_CAP_NONE;
        if (r == first && first_cap) cap = TS_CAP_TOP;
        if (r == last && last_cap) cap = TS_CAP_BOTTOM;
        plot_cell(out_map, col, r, inv, TS_TILE_FULL(shade, cap, border));
    }
}

static void draw_segment_column(uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS],
                                uint8_t col, uint8_t wall_id, uint8_t profile,
                                uint8_t inv, uint8_t shade, uint8_t border,
                                int8_t inv_step) {
    int16_t top, bottom;
    uint8_t snap_top, snap_bottom;
    int8_t top_step, bottom_step;
    int16_t top_l, top_r, bot_l, bot_r;
    int8_t top_min_row, top_max_row, bot_min_row, bot_max_row;
    int8_t r;

    vertical_profile(profile, inv, &top, &bottom, &snap_top, &snap_bottom);

    /* Reciprocal depth varies across a projected wall. Convert that derivative
     * directly into a tiny signed boundary vector over each eight-pixel tile.
     * The LUT only needs five slopes (-2..+2 px/tile). */
    top_step = clamp_s8(-((int16_t)inv_step >> 1), TS_EDGE_SLOPE_MIN,
                        (int8_t)(TS_EDGE_SLOPE_MIN + TS_EDGE_SLOPE_COUNT - 1));
    if (profile == TS_PROFILE_RAISED_FULL)
        bottom_step = clamp_s8(((int16_t)inv_step >> 1) - ((int16_t)inv_step >> 3),
                               TS_EDGE_SLOPE_MIN,
                               (int8_t)(TS_EDGE_SLOPE_MIN + TS_EDGE_SLOPE_COUNT - 1));
    else
        bottom_step = clamp_s8((int16_t)inv_step >> 1, TS_EDGE_SLOPE_MIN,
                               (int8_t)(TS_EDGE_SLOPE_MIN + TS_EDGE_SLOPE_COUNT - 1));

    if (snap_top) {
        top = (int16_t)((top + 4) & ~7);
        top_step = 0;
    }
    if (snap_bottom) {
        bottom = (int16_t)(((bottom + 4) & ~7) - 1);
        bottom_step = 0;
    }

    top = clamp_u8(top, 143u);
    bottom = clamp_u8(bottom, 143u);
    if (top > bottom) return;

    if (inv > cols[col].invz) {
        cols[col].invz = inv;
        cols[col].wall_id = wall_id;
        cols[col].shade = shade;
        cols[col].top = (uint8_t)top;
        cols[col].bottom = (uint8_t)bottom;
        cols[col].top_step = top_step;
        cols[col].bottom_step = bottom_step;
    }

    if (snap_top) {
        top_min_row = top_max_row = row_floor(top);
    } else {
        top_l = (int16_t)(top - (top_step >> 1));
        top_r = (int16_t)(top_l + top_step);
        top_min_row = row_floor(top_l < top_r ? top_l : top_r);
        top_max_row = row_floor(top_l > top_r ? top_l : top_r);
        for (r = top_min_row; r <= top_max_row; ++r) {
            int16_t local = (int16_t)(top_l - ((int16_t)r << 3));
            plot_cell(out_map, col, r, inv, edge_entry(shade, local, top_step, 0u));
        }
    }

    if (snap_bottom) {
        bot_min_row = bot_max_row = row_floor(bottom);
    } else {
        bot_l = (int16_t)(bottom - (bottom_step >> 1));
        bot_r = (int16_t)(bot_l + bottom_step);
        bot_min_row = row_floor(bot_l < bot_r ? bot_l : bot_r);
        bot_max_row = row_floor(bot_l > bot_r ? bot_l : bot_r);
        for (r = bot_min_row; r <= bot_max_row; ++r) {
            int16_t local = (int16_t)(bot_l - ((int16_t)r << 3));
            plot_cell(out_map, col, r, inv, edge_entry(shade, local, bottom_step, 1u));
        }
    }

    if (snap_top && snap_bottom) {
        draw_full_span(out_map, col, top_min_row, bot_max_row, inv, shade, border, 1u, 1u);
    } else if (snap_top) {
        draw_full_span(out_map, col, top_min_row, (int8_t)(bot_min_row - 1), inv, shade, border, 1u, 0u);
    } else if (snap_bottom) {
        draw_full_span(out_map, col, (int8_t)(top_max_row + 1), bot_max_row, inv, shade, border, 0u, 1u);
    } else {
        draw_full_span(out_map, col, (int8_t)(top_max_row + 1), (int8_t)(bot_min_row - 1),
                       inv, shade, border, 0u, 0u);
    }
}

static void clear_frame(uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    uint16_t i;
    uint8_t c, y;
    for (i = 0u; i < TS_MAP_CELLS; ++i) g_depth[i] = 0u;
    for (y = 0u; y < TS_ROWS; ++y) {
        uint16_t base = (y < 9u) ? TS_TILE_CEILING : (y == 9u ? TS_TILE_HORIZON : TS_TILE_FLOOR);
        uint16_t row = (uint16_t)y * TS_COLS;
        for (c = 0u; c < TS_COLS; ++c) out_map[row + c] = base;
    }
    for (c = 0u; c < TS_COLS; ++c) {
        cols[c].invz = 0u;
        cols[c].wall_id = TS_NO_WALL;
        cols[c].shade = 0u;
        cols[c].top = TS_HORIZON;
        cols[c].bottom = TS_HORIZON;
        cols[c].top_step = 0;
        cols[c].bottom_step = 0;
    }
}

void ts_render_columns(const TSState *s, TSColumn cols[TS_COLS]) {
    uint16_t dummy[TS_MAP_CELLS];
    ts_build_tilemap(s, dummy, cols);
}

void ts_build_tilemap(const TSState *s, uint16_t out_map[TS_MAP_CELLS], TSColumn cols[TS_COLS]) {
    int16_t cam_x[TS_VERTICES];
    int16_t cam_z[TS_VERTICES];
    uint8_t vi, si;
    int16_t px = (int16_t)(s->x_q4 >> 4);
    int16_t py = (int16_t)(s->y_q4 >> 4);
    int8_t sn = k_sin[s->yaw];
    int8_t cs = k_sin[(uint8_t)(s->yaw + 64u)];

    clear_frame(out_map, cols);

    for (vi = 0u; vi < TS_VERTICES; ++vi) {
        int16_t dx = (int16_t)k_vertices[vi].x - px;
        int16_t dy = (int16_t)k_vertices[vi].y - py;
        cam_z[vi] = (int16_t)(((int16_t)dx * cs + (int16_t)dy * sn) >> 7);
        cam_x[vi] = (int16_t)((-(int16_t)dx * sn + (int16_t)dy * cs) >> 7);
    }

    /* No ray-per-screen-X traversal: each world segment is projected once into
     * a screen interval. Only the coarse columns/cells it contests are visited. */
    for (si = 0u; si < TS_SEGMENTS; ++si) {
        const TSSegment *seg = &k_segments[si];
        int16_t x0 = cam_x[seg->a], z0 = cam_z[seg->a];
        int16_t x1 = cam_x[seg->b], z1 = cam_z[seg->b];
        int16_t sx0, sx1;
        uint8_t inv0, inv1;
        int8_t c0, c1, cc;
        uint8_t span;
        int16_t inv_q6, step_q6;

        if (z0 < TS_NEAR_Z && z1 < TS_NEAR_Z) continue;
        if (z0 < TS_NEAR_Z) { x0 = clip_x_q7(x0, z0, x1, z1); z0 = TS_NEAR_Z; }
        if (z1 < TS_NEAR_Z) { x1 = clip_x_q7(x1, z1, x0, z0); z1 = TS_NEAR_Z; }

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
                uint8_t border = 0u;
                uint8_t shade = shade_for(inv, seg->shade_bias);
                int8_t inv_step = clamp_s8((int16_t)(step_q6 >> 6), -8, 8);
                if (cc == c0 && c0 >= 0) border |= 1u;
                if (cc == c1 && c1 < (int8_t)TS_COLS) border |= 2u;
                draw_segment_column(out_map, cols, (uint8_t)cc, si, seg->profile,
                                    inv, shade, border, inv_step);
            }
            inv_q6 = (int16_t)(inv_q6 + step_q6);
        }
    }
}
