#include <stdint.h>
#include <gbdk/platform.h>
#include "sim.h"
#include "tiles.h"

#define DEFAULT_TICK_DIV 1u
#define WORLD_BLOCK_W ((SIM_W + 1u) / 2u)  /* 23 hardware tiles */
#define WORLD_BLOCK_H ((SIM_H + 1u) / 2u)  /* 12 hardware tiles */
#define WORLD_TILE_COUNT (WORLD_BLOCK_W * WORLD_BLOCK_H)
#define MAP_TILE_Y 2u
#define HUD_TILE_BASE 300u
#define HUD_FONT_COUNT 18u
#define SPRITE_TILE_BLUE 0u
#define SPRITE_TILE_RED 1u
#define SPRITE_TILE_HIT 2u
#define SPRITE_TILE_SUPPRESSED 3u
#define CAMERA_MAX_X ((SIM_W * 4u) - 160u) /* 184 - 160 = 24 pixels */
#define MAP_SCROLL_SCANLINE ((((MAP_TILE_Y + DEVICE_SCREEN_Y_OFFSET) * 8u)) - 2u)

static Sim g_sim;
static uint16_t g_last_hud[40u];
static uint8_t g_frame_counter;
static uint8_t g_tick_div = DEFAULT_TICK_DIV;
static uint8_t g_paused;
static uint8_t g_prev_pad;
static uint8_t g_camera_x;
static uint8_t g_camera_target_x;
#ifndef DEFAULT_SEED
#define DEFAULT_SEED 42u
#endif
static uint16_t g_seed = DEFAULT_SEED;
static uint8_t g_marker_tiles[4u * 32u];
static uint8_t g_world_pattern_keys[32u];
static uint8_t g_world_pattern_count;

static const palette_color_t k_palette[16] = {
    RGB(0, 0, 0),      /* 0 transparent / black */
    RGB(1, 2, 4),      /* 1 floor */
    RGB(7, 8, 9),      /* 2 wall */
    RGB(15, 9, 1),     /* 3 closed door */
    RGB(6, 4, 1),      /* 4 open door */
    RGB(1, 12, 15),    /* 5 blue team */
    RGB(15, 2, 2),     /* 6 red team */
    RGB(15, 15, 15),   /* 7 hit */
    RGB(15, 15, 1),    /* 8 tracer */
    RGB(0, 5, 8),      /* 9 dim blue */
    RGB(7, 0, 0),      /* 10 dim red */
    RGB(10, 10, 10),   /* 11 text */
    RGB(5, 15, 5),     /* 12 event */
    RGB(11, 1, 12),    /* 13 suppressed */
    RGB(2, 2, 2),      /* 14 spare */
    RGB(15, 15, 15)    /* 15 font */
};

static uint8_t cell_kind_at(uint8_t x, uint8_t y) {
    return sim_cell_kind(&g_sim, x, y);
}

static uint8_t terrain_color_from_kind(uint8_t kind) {
    if (kind == CELL_WALL) return 2u;
    if (kind == CELL_DOOR_CLOSED) return 3u;
    if (kind == CELL_DOOR_OPEN) return 4u;
    return 1u;
}

static void paint_pixel(uint8_t *tile, uint8_t x, uint8_t y, uint8_t color) {
    uint8_t p;
    uint8_t bit = (uint8_t)(0x80u >> x);
    uint8_t *row = tile + (uint16_t)y * 4u;
    for (p = 0u; p < 4u; ++p) {
        if (color & (uint8_t)(1u << p)) row[p] |= bit;
    }
}

static uint8_t world_pattern_key(uint8_t bx, uint8_t by) {
    uint8_t key = 0u, sx, sy;
    for (sy = 0u; sy < 2u; ++sy)
        for (sx = 0u; sx < 2u; ++sx)
            key |= (uint8_t)(cell_kind_at((uint8_t)(bx * 2u + sx),
                                         (uint8_t)(by * 2u + sy)) << ((sy * 2u + sx) * 2u));
    return key;
}

static void make_world_pattern_key(uint8_t key, uint8_t *tile) {
    uint8_t sx, sy, px, py, i;
    for (i = 0u; i < 32u; ++i) tile[i] = 0u;
    for (sy = 0u; sy < 2u; ++sy) {
        for (sx = 0u; sx < 2u; ++sx) {
            uint8_t shift = (uint8_t)((sy * 2u + sx) * 2u);
            uint8_t kind = (uint8_t)((key >> shift) & 3u);
            uint8_t color = terrain_color_from_kind(kind);
            for (py = 0u; py < 4u; ++py)
                for (px = 0u; px < 4u; ++px)
                    paint_pixel(tile, (uint8_t)(sx * 4u + px), (uint8_t)(sy * 4u + py), color);
        }
    }
}

/* The exact office-loop needs only fourteen distinct 2x2 terrain patterns. */
static uint16_t pattern_for_block(uint8_t bx, uint8_t by) {
    uint8_t key = world_pattern_key(bx, by);
    uint8_t i;
    for (i = 0u; i < g_world_pattern_count; ++i)
        if (g_world_pattern_keys[i] == key) return i;

    if (g_world_pattern_count < 32u) {
        uint8_t tile[32u];
        uint8_t ti = g_world_pattern_count++;
        g_world_pattern_keys[ti] = key;
        make_world_pattern_key(key, tile);
        set_bkg_4bpp_data(ti, 1u, tile);
        return ti;
    }
    return 0u; /* defensive; authored map is far below the cache limit */
}

static void upload_world_block(uint8_t bx, uint8_t by) {
    uint16_t ti = pattern_for_block(bx, by);
    set_attributed_tile_xy(bx, (uint8_t)(MAP_TILE_Y + by), ti);
}

static void upload_world(void) {
    uint8_t bx, by;
    for (by = 0u; by < WORLD_BLOCK_H; ++by)
        for (bx = 0u; bx < WORLD_BLOCK_W; ++bx)
            upload_world_block(bx, by);
}

static uint16_t hud_font_tile(char c) {
    uint8_t src = gg_font_tile(c);
    return (uint16_t)(HUD_TILE_BASE + (uint16_t)(src - TILE_FONT_SPACE));
}

static void init_marker_tile(uint8_t tile_id, uint8_t color) {
    uint8_t *tile = g_marker_tiles + (uint16_t)tile_id * 32u;
    uint8_t i, x, y;
    for (i = 0u; i < 32u; ++i) tile[i] = 0u;
    for (y = 2u; y < 6u; ++y)
        for (x = 2u; x < 6u; ++x)
            paint_pixel(tile, x, y, color);
}

/*
 * Game Gear's visible image starts 24 VDP scanlines down, so the SMS hardware's
 * built-in "lock top 16 lines" bit is offscreen. Use one line interrupt instead:
 * VBlank resets scroll to zero for the HUD, then scanline 38 applies the camera
 * for the battlefield below it.
 */
static void vblank_scroll_isr(void) {
    __WRITE_VDP_REG_UNSAFE(VDP_R10, MAP_SCROLL_SCANLINE);
    __WRITE_VDP_REG_UNSAFE(VDP_RSCX, 0u);
}

static void scanline_scroll_isr(void) {
    __WRITE_VDP_REG_UNSAFE(VDP_RSCX, (uint8_t)(0u - g_camera_x));
    __WRITE_VDP_REG_UNSAFE(VDP_R10, R10_INT_OFF);
}

static void init_video_assets(void) {
    uint8_t x, y;
    g_world_pattern_count = 0u;
    set_bkg_palette(0u, 1u, k_palette);
    set_sprite_palette(0u, 1u, k_palette);

    /* Re-use the compact stage-3 glyph patterns, but move them above world patterns. */
    set_bkg_4bpp_data(HUD_TILE_BASE, HUD_FONT_COUNT,
                      gg_tile_data + (uint16_t)TILE_FONT_SPACE * 32u);

    init_marker_tile(SPRITE_TILE_BLUE, 5u);
    init_marker_tile(SPRITE_TILE_RED, 6u);
    init_marker_tile(SPRITE_TILE_HIT, 7u);
    init_marker_tile(SPRITE_TILE_SUPPRESSED, 13u);
    set_sprite_4bpp_data(0u, 4u, g_marker_tiles);

    /* Clear the whole region that can become visible during the 24-pixel pan. */
    for (y = 0u; y < 18u; ++y)
        for (x = 0u; x < WORLD_BLOCK_W; ++x)
            set_attributed_tile_xy(x, y, hud_font_tile(' '));

    upload_world();

    CRITICAL {
        add_LCD(scanline_scroll_isr);
        add_VBL(vblank_scroll_isr);
    }
    set_interrupts(VBL_IFLAG | LCD_IFLAG);
    __WRITE_VDP_REG(VDP_RSCX, 0u);
}

static void invalidate_hud(void) {
    uint8_t i;
    for (i = 0u; i < 40u; ++i) g_last_hud[i] = 0xffffu;
}

static void render_hud(void) {
    uint16_t t = g_sim.tick;
    uint8_t i;
    uint16_t hud[40u];
    uint8_t ba = sim_team_alive(&g_sim, TEAM_BLUE);
    uint8_t ra = sim_team_alive(&g_sim, TEAM_RED);
    uint8_t bh = sim_team_hp(&g_sim, TEAM_BLUE);
    uint8_t rh = sim_team_hp(&g_sim, TEAM_RED);

    for (i = 0u; i < 40u; ++i) hud[i] = hud_font_tile(' ');

    /* Row 0: B4H20 T0057 R6H30 */
    hud[0u] = hud_font_tile('B');
    hud[1u] = hud_font_tile((char)('0' + ba));
    hud[2u] = hud_font_tile('H');
    hud[3u] = hud_font_tile((char)('0' + ((bh / 10u) % 10u)));
    hud[4u] = hud_font_tile((char)('0' + (bh % 10u)));
    hud[6u] = hud_font_tile('T');
    hud[7u] = hud_font_tile((char)('0' + ((t / 1000u) % 10u)));
    hud[8u] = hud_font_tile((char)('0' + ((t / 100u) % 10u)));
    hud[9u] = hud_font_tile((char)('0' + ((t / 10u) % 10u)));
    hud[10u] = hud_font_tile((char)('0' + (t % 10u)));
    hud[12u] = hud_font_tile('R');
    hud[13u] = hud_font_tile((char)('0' + ra));
    hud[14u] = hud_font_tile('H');
    hud[15u] = hud_font_tile((char)('0' + ((rh / 10u) % 10u)));
    hud[16u] = hud_font_tile((char)('0' + (rh % 10u)));

    /* Row 1: camera + original population count + pause state. */
    hud[20u] = hud_font_tile('C');
    hud[21u] = hud_font_tile((char)('0' + ((g_camera_x / 10u) % 10u)));
    hud[22u] = hud_font_tile((char)('0' + (g_camera_x % 10u)));
    hud[24u] = hud_font_tile('B');
    hud[25u] = hud_font_tile((char)('0' + SIM_BLUE_COUNT));
    hud[27u] = hud_font_tile('R');
    hud[28u] = hud_font_tile((char)('0' + g_sim.red_count));
    hud[30u] = hud_font_tile('P');
    hud[31u] = hud_font_tile(g_paused ? '1' : '0');
    if (g_sim.done) hud[38u] = hud_font_tile('H');

    for (i = 0u; i < 40u; ++i) {
        if (hud[i] != g_last_hud[i]) {
            set_attributed_tile_xy((uint8_t)(i % 20u), (uint8_t)(i / 20u), hud[i]);
            g_last_hud[i] = hud[i];
        }
    }
}

static void update_camera_target(void) {
    uint8_t i, n = 0u;
    uint16_t sum_x = 0u;
    int16_t focus_px, target;

    for (i = 0u; i < SIM_BLUE_COUNT; ++i) {
        if (!g_sim.agents[i].alive) continue;
        sum_x = (uint16_t)(sum_x + g_sim.agents[i].x);
        ++n;
    }
    if (!n) {
        for (i = SIM_BLUE_COUNT; i < g_sim.agent_count; ++i) {
            if (!g_sim.agents[i].alive) continue;
            sum_x = (uint16_t)(sum_x + g_sim.agents[i].x);
            ++n;
        }
    }
    focus_px = n ? (int16_t)(((sum_x / n) * 4u) + 2u) : 80;

    /* A live firefight tugs the camera slightly toward the newest combat event. */
    if (g_sim.event_count) {
        const uint8_t ei = (uint8_t)(g_sim.event_count - 1u);
        const uint8_t kind = g_sim.events[ei].kind;
        if (kind == EVENT_SHOT_MISS || kind == EVENT_SHOT_HIT || kind == EVENT_KILL)
            focus_px = (int16_t)((focus_px * 3 + (int16_t)g_sim.events[ei].x * 4 + 2) / 4);
    }

    target = (int16_t)(focus_px - 80);
    if (target < 0) target = 0;
    if (target > (int16_t)CAMERA_MAX_X) target = CAMERA_MAX_X;
    g_camera_target_x = (uint8_t)target;
}

static void step_camera(void) {
    if (g_camera_x < g_camera_target_x) ++g_camera_x;
    else if (g_camera_x > g_camera_target_x) --g_camera_x;
}

static void render_sprites(void) {
    uint8_t i;
    for (i = 0u; i < SIM_MAX_AGENTS; ++i) {
        if (i >= g_sim.agent_count || !g_sim.agents[i].alive) {
            hide_sprite(i);
            continue;
        }
        {
            const Agent *a = &g_sim.agents[i];
            int16_t sx = (int16_t)a->x * 4 + 2 - (int16_t)g_camera_x;
            int16_t sy = (int16_t)MAP_TILE_Y * 8 + (int16_t)a->y * 4 + 2;
            uint8_t tile;
            if (sx < -6 || sx > 165 || sy < -6 || sy > 149) {
                hide_sprite(i);
                continue;
            }
            if (a->hit_flash) tile = SPRITE_TILE_HIT;
            else if (a->suppressed) tile = SPRITE_TILE_SUPPRESSED;
            else tile = sim_agent_team(i) == TEAM_BLUE ? SPRITE_TILE_BLUE : SPRITE_TILE_RED;
            set_sprite_tile(i, tile);
            move_sprite(i,
                        (uint8_t)(DEVICE_SPRITE_PX_OFFSET_X + sx - 4),
                        (uint8_t)(DEVICE_SPRITE_PX_OFFSET_Y + sy - 4));
        }
    }
}

static void process_terrain_events(void) {
    uint8_t i;
    for (i = 0u; i < g_sim.event_count; ++i) {
        if (g_sim.events[i].kind == EVENT_DOOR)
            upload_world_block((uint8_t)(g_sim.events[i].x >> 1),
                               (uint8_t)(g_sim.events[i].y >> 1));
    }
}

static void restart(uint16_t seed) {
    g_seed = seed ? seed : 1u;
    sim_init(&g_sim, g_seed);
    g_frame_counter = 0u;
    g_paused = 0u;
    g_camera_x = 0u;
    g_camera_target_x = 0u;
    invalidate_hud();
    upload_world();
    update_camera_target();
    render_hud();
    render_sprites();
}

static void handle_input(void) {
    uint8_t pad = joypad();
    uint8_t pressed = (uint8_t)(pad & (uint8_t)~g_prev_pad);
    g_prev_pad = pad;

    if (pressed & J_START) { g_paused ^= 1u; invalidate_hud(); }
    if (pressed & J_A) restart((uint16_t)(g_seed + 1u));
    if (pressed & J_B) {
        if (g_tick_div == 1u) g_tick_div = 2u;
        else if (g_tick_div == 2u) g_tick_div = 4u;
        else g_tick_div = 1u;
        invalidate_hud();
    }
    if (g_paused && (pressed & J_RIGHT) && !g_sim.done) {
        sim_tick(&g_sim);
        process_terrain_events();
        update_camera_target();
        invalidate_hud();
    }
}

void main(void) {
    DISPLAY_OFF;
    HIDE_SPRITES;
    SET_BORDER_COLOR(0u);
    SPRITES_8x8;
    sim_init(&g_sim, g_seed);
    invalidate_hud();
    init_video_assets();
    update_camera_target();
    render_hud();
    render_sprites();
    SHOW_SPRITES;
    DISPLAY_ON;

    for (;;) {
        vsync();
        handle_input();

        if (!g_paused && !g_sim.done) {
            ++g_frame_counter;
            if (g_frame_counter >= g_tick_div) {
                g_frame_counter = 0u;
                sim_tick(&g_sim);
                process_terrain_events();
                update_camera_target();
                invalidate_hud();
            }
        }

        step_camera();
        render_sprites();
        render_hud();
    }
}
