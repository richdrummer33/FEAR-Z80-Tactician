#if defined(__SDCC)
#pragma bank 1
#endif

#include "vfx_lab.h"

#define SCENE_BULLETS_FRAMES 240u
#define SCENE_BEAM_FRAMES 300u
#define SCENE_GRENADE_FRAMES 360u
#define SCENE_FLASH_FRAMES 240u
#define SCENE_STRESS_FRAMES 420u

static int16_t cell_px(uint8_t c) { return (int16_t)c * 4 + 2; }
static int16_t iabs16(int16_t v) { return v < 0 ? (int16_t)-v : v; }

static uint8_t world_solid(int16_t px, int16_t py, void *ctx) {
    Sim *s = (Sim *)ctx;
    int16_t cx, cy;
    if (px < 0 || py < 0) return 1u;
    cx = px >> 2;
    cy = py >> 2;
    if (cx < 0 || cy < 0 || cx >= (int16_t)SIM_W || cy >= (int16_t)SIM_H) return 1u;
    return sim_cell_passable(s, (uint8_t)cx, (uint8_t)cy, 0u) ? 0u : 1u;
}

static void clear_projectiles(VfxLab *lab) {
    uint8_t i;
    for (i = 0u; i < VFX_PROJECTILE_MAX; ++i) lab->projectiles[i].active = 0u;
}

static void clear_debris(VfxLab *lab) {
    uint8_t i;
    for (i = 0u; i < FX_MAX_DEBRIS; ++i) lab->debris[i].active = 0u;
}

static void clear_beam(VfxLab *lab) {
    lab->beam.phase = VFX_BEAM_OFF;
    lab->beam.cell_count = 0u;
    lab->beam.formed_count = 0u;
    lab->beam.decay_count = 0u;
    lab->beam.hold_frames = 0u;
    lab->beam.phase_tick = 0u;
    lab->beam.duty_numer = 3u;
}

static uint16_t scene_duration(uint8_t scene) {
    if (scene == VFX_SCENE_BULLETS) return SCENE_BULLETS_FRAMES;
    if (scene == VFX_SCENE_BEAM) return SCENE_BEAM_FRAMES;
    if (scene == VFX_SCENE_GRENADE) return SCENE_GRENADE_FRAMES;
    if (scene == VFX_SCENE_FLASHBANG) return SCENE_FLASH_FRAMES;
    return SCENE_STRESS_FRAMES;
}

void vfx_lab_init(VfxLab *lab, uint16_t seed) {
    uint8_t i;
    lab->scene = VFX_SCENE_BULLETS;
    lab->scene_frame = 0u;
    lab->total_frame = 0u;
    lab->rng = seed ? seed : 0xA5E1u;
    lab->phase3 = 0u;
    clear_projectiles(lab);
    clear_beam(lab);
    clear_debris(lab);
    lab->impact_active = 0u;
    lab->ring_active = 0u;
    lab->shake_x = lab->shake_y = 0;
    lab->shake_impulse_x = lab->shake_impulse_y = 0;
    lab->shake_age = lab->shake_duration = 0u;
    lab->grenade_age = lab->flashbang_age = 0u;
    lab->flicker_age = lab->flicker_duration = lab->blackout_run = 0u;
    lab->exposure = VFX_EXPOSURE_NORMAL;
    lab->blackout = 0u;
    lab->blackout_white = 0u;
    lab->raster_warp_active = 0u;
    lab->raster_warp_center_y = 0u;
    lab->raster_warp_radius = 0u;
    lab->raster_warp_strength = 0;
    lab->ai_tick_div_hint = 1u;
    lab->work_units_hint = 2u;
    lab->vram_budget_hint = 96u;
    for (i = 0u; i < VFX_BEAM_MAX_CELLS; ++i) {
        lab->beam.cells[i].bx = lab->beam.cells[i].by = 0u;
        lab->beam.cells[i].lx0 = lab->beam.cells[i].ly0 = 0u;
        lab->beam.cells[i].lx1 = lab->beam.cells[i].ly1 = 0u;
    }
}

void vfx_lab_set_scene(VfxLab *lab, uint8_t scene) {
    if (scene >= VFX_SCENE_COUNT) scene = 0u;
    lab->scene = scene;
    lab->scene_frame = 0u;
    clear_projectiles(lab);
    clear_beam(lab);
    clear_debris(lab);
    lab->impact_active = 0u;
    lab->ring_active = 0u;
    lab->shake_x = lab->shake_y = 0;
    lab->shake_age = lab->shake_duration = 0u;
    lab->grenade_age = lab->flashbang_age = 0u;
    lab->flicker_age = lab->flicker_duration = lab->blackout_run = 0u;
    lab->exposure = VFX_EXPOSURE_NORMAL;
    lab->blackout = 0u;
    lab->blackout_white = 0u;
    lab->raster_warp_active = 0u;
    lab->ai_tick_div_hint = 1u;
}

void vfx_lab_next_scene(VfxLab *lab) {
    vfx_lab_set_scene(lab, (uint8_t)((lab->scene + 1u) == VFX_SCENE_COUNT ? 0u : lab->scene + 1u));
}

static void spawn_projectile(VfxLab *lab, int16_t sx, int16_t sy,
                             int16_t tx, int16_t ty, uint8_t slot) {
    int16_t dx = (int16_t)(tx - sx);
    int16_t dy = (int16_t)(ty - sy);
    int16_t mag = iabs16(dx) > iabs16(dy) ? iabs16(dx) : iabs16(dy);
    uint8_t frames;
    int16_t vx, vy;
    if (slot >= VFX_PROJECTILE_MAX) return;
    frames = (uint8_t)(mag / 5);
    if (frames < 8u) frames = 8u;
    if (frames > 36u) frames = 36u;
    vx = (int16_t)((dx << FX_FP_SHIFT) / frames);
    vy = (int16_t)((dy << FX_FP_SHIFT) / frames);
    fx_projectile_init(&lab->projectiles[slot], (int16_t)(sx << FX_FP_SHIFT),
                       (int16_t)(sy << FX_FP_SHIFT), vx, vy,
                       (uint8_t)(frames + 5u), (uint8_t)lab->rng);
}

static void build_beam_cells(VfxBeam *b, int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    int16_t dx = iabs16((int16_t)(x1 - x0));
    int16_t sx = x0 < x1 ? 1 : -1;
    int16_t dy = (int16_t)-iabs16((int16_t)(y1 - y0));
    int16_t sy = y0 < y1 ? 1 : -1;
    int16_t err = (int16_t)(dx + dy);
    uint8_t last_bx = 0xffu, last_by = 0xffu;
    b->cell_count = 0u;
    for (;;) {
        if (x0 >= 0 && y0 >= 0) {
            uint8_t bx = (uint8_t)(x0 >> 3);
            uint8_t by = (uint8_t)(y0 >> 3);
            uint8_t lx = (uint8_t)(x0 & 7);
            uint8_t ly = (uint8_t)(y0 & 7);
            if (bx != last_bx || by != last_by) {
                if (b->cell_count < VFX_BEAM_MAX_CELLS) {
                    VfxBeamCell *c = &b->cells[b->cell_count++];
                    c->bx = bx; c->by = by;
                    c->lx0 = c->lx1 = lx;
                    c->ly0 = c->ly1 = ly;
                }
                last_bx = bx; last_by = by;
            } else if (b->cell_count) {
                VfxBeamCell *c = &b->cells[b->cell_count - 1u];
                c->lx1 = lx; c->ly1 = ly;
            }
        }
        if (x0 == x1 && y0 == y1) break;
        {
            int16_t e2 = (int16_t)(err << 1);
            if (e2 >= dy) { err = (int16_t)(err + dy); x0 = (int16_t)(x0 + sx); }
            if (e2 <= dx) { err = (int16_t)(err + dx); y0 = (int16_t)(y0 + sy); }
        }
    }
}

void vfx_lab_fire_beam(VfxLab *lab, int16_t source_x, int16_t source_y,
                       int16_t hit_x, int16_t hit_y) {
    clear_beam(lab);
    lab->beam.x0 = source_x; lab->beam.y0 = source_y;
    lab->beam.x1 = hit_x; lab->beam.y1 = hit_y;
    build_beam_cells(&lab->beam, source_x, source_y, hit_x, hit_y);
    lab->beam.phase = VFX_BEAM_FORMING;
    lab->beam.phase_tick = 0u;
    lab->beam.duty_numer = 3u;
    lab->impact_x = hit_x;
    lab->impact_y = hit_y;
    lab->impact_active = 10u;
}

static void spawn_debris(VfxLab *lab, int16_t x, int16_t y) {
    static const int8_t dirs[16][2] = {
        { 16,  0},{ 15,  6},{ 11, 11},{  6, 15},
        {  0, 16},{ -6, 15},{-11, 11},{-15,  6},
        {-16,  0},{-15, -6},{-11,-11},{ -6,-15},
        {  0,-16},{  6,-15},{ 11,-11},{ 15, -6}
    };
    uint8_t i;
    for (i = 0u; i < FX_MAX_DEBRIS; ++i) {
        uint8_t cls;
        int16_t speed, vx, vy, vz;
        lab->rng = fx_lfsr16(lab->rng);
        cls = ((i + (uint8_t)lab->rng) & 3u) == 0u ? FX_DEBRIS_CHUNK : FX_DEBRIS_SMALL;
        speed = cls == FX_DEBRIS_CHUNK ? (int16_t)(38 + (lab->rng & 15u))
                                       : (int16_t)(58 + (lab->rng & 31u));
        vx = (int16_t)(((int16_t)dirs[i][0] * speed) >> 4);
        vy = (int16_t)(((int16_t)dirs[i][1] * speed) >> 4);
        vz = cls == FX_DEBRIS_CHUNK ? (int16_t)(40 + ((lab->rng >> 4) & 15u))
                                    : (int16_t)(56 + ((lab->rng >> 4) & 31u));
        fx_debris_init(&lab->debris[i], cls,
                       (int16_t)(x << FX_FP_SHIFT), (int16_t)(y << FX_FP_SHIFT),
                       vx, vy, vz, cls == FX_DEBRIS_CHUNK ? 150u : 105u,
                       (uint8_t)lab->rng);
    }
}

void vfx_lab_detonate_grenade(VfxLab *lab, int16_t x, int16_t y,
                              int8_t impulse_x, int8_t impulse_y) {
    lab->grenade_age = 1u;
    lab->ring_x = x; lab->ring_y = y;
    lab->ring_active = 1u;
    lab->ring_radius = 1u;
    lab->ring_phase = lab->phase3;
    lab->impact_x = x; lab->impact_y = y;
    lab->impact_active = 8u;
    lab->shake_impulse_x = impulse_x;
    lab->shake_impulse_y = impulse_y;
    lab->shake_age = 1u;
    lab->shake_duration = 120u;
    lab->flicker_age = 1u;
    lab->flicker_duration = 120u;
    lab->blackout_run = 0u;
    lab->ai_tick_div_hint = 0u;
    spawn_debris(lab, x, y);
}

void vfx_lab_trigger_flashbang(VfxLab *lab, int16_t x, int16_t y) {
    lab->flashbang_age = 1u;
    lab->impact_x = x; lab->impact_y = y;
    lab->impact_active = 6u;
    lab->flicker_age = 1u;
    lab->flicker_duration = 100u;
    lab->blackout_run = 0u;
    lab->shake_impulse_x = 2;
    lab->shake_impulse_y = -2;
    lab->shake_age = 1u;
    lab->shake_duration = 50u;
    lab->ai_tick_div_hint = 0u;
}

static void update_beam(VfxLab *lab) {
    VfxBeam *b = &lab->beam;
    if (b->phase == VFX_BEAM_OFF) return;
    ++b->phase_tick;
    if (b->phase == VFX_BEAM_FORMING) {
        if (b->formed_count < b->cell_count) ++b->formed_count;
        if (b->formed_count >= b->cell_count) {
            b->phase = VFX_BEAM_HOLD;
            b->phase_tick = 0u;
            b->hold_frames = 18u;
        }
    } else if (b->phase == VFX_BEAM_HOLD) {
        b->duty_numer = 3u;
        if (b->phase_tick >= b->hold_frames) {
            b->phase = VFX_BEAM_DECAY;
            b->phase_tick = 0u;
            b->decay_count = 0u;
            b->duty_numer = 2u;
        }
    } else {
        if (b->phase_tick > 15u) b->duty_numer = 1u;
        if (!(b->phase_tick & 1u) && b->decay_count < b->cell_count) ++b->decay_count;
        if (b->decay_count >= b->cell_count) clear_beam(lab);
    }
}

static void update_projectiles(VfxLab *lab) {
    uint8_t i;
    for (i = 0u; i < VFX_PROJECTILE_MAX; ++i) fx_projectile_tick(&lab->projectiles[i]);
}

static void update_debris(VfxLab *lab, Sim *sim) {
    uint8_t i;
    /* fx_debris_tick and world_solid are deliberately co-located in bank 1 on GG.
       The function pointer is therefore a same-bank address, not a far pointer. */
    for (i = 0u; i < FX_MAX_DEBRIS; ++i)
        if (lab->debris[i].active) fx_debris_tick(&lab->debris[i], world_solid, sim);
}

static void update_shake(VfxLab *lab) {
    uint8_t amp;
    int8_t rx, ry;
    if (!lab->shake_age || lab->shake_age > lab->shake_duration) {
        lab->shake_x = lab->shake_y = 0;
        return;
    }
    lab->rng = fx_lfsr16(lab->rng);
    if (lab->shake_age <= 6u) amp = 4u;
    else if (lab->shake_age <= 24u) amp = 2u;
    else if (lab->shake_age <= 70u) amp = 1u;
    else amp = (lab->shake_age & 3u) == 0u ? 1u : 0u;
    rx = (int8_t)((lab->rng & 3u) - 1u);
    ry = (int8_t)(((lab->rng >> 2) & 3u) - 1u);
    if (lab->shake_age <= 5u) {
        lab->shake_x = (int8_t)(lab->shake_impulse_x + rx);
        lab->shake_y = (int8_t)(lab->shake_impulse_y + ry);
    } else {
        lab->shake_x = (int8_t)(rx * (int8_t)amp);
        lab->shake_y = (int8_t)(ry * (int8_t)amp);
    }
    ++lab->shake_age;
}

static void update_flicker(VfxLab *lab) {
    uint8_t threshold;
    uint8_t sample;
    uint8_t quarter;
    lab->blackout = 0u;
    lab->blackout_white = 0u;
    if (!lab->flicker_age || lab->flicker_age > lab->flicker_duration) return;
    lab->rng = fx_lfsr16(lab->rng);
    sample = (uint8_t)lab->rng;

    /* Piecewise decay avoids a 16-bit divide every frame on Z80. */
    quarter = (uint8_t)(lab->flicker_duration >> 2);
    if (lab->flicker_age <= quarter) threshold = 112u;
    else if (lab->flicker_age <= (uint8_t)(quarter << 1)) threshold = 76u;
    else if (lab->flicker_age <= (uint8_t)(quarter + (quarter << 1))) threshold = 42u;
    else threshold = 16u;

    if (sample < threshold && lab->blackout_run < 2u) {
        lab->blackout = 1u;
        ++lab->blackout_run;
    } else lab->blackout_run = 0u;
    ++lab->flicker_age;
}

static void update_grenade(VfxLab *lab) {
    uint8_t a = lab->grenade_age;
    if (!a) return;

    if (a <= 2u) lab->exposure = VFX_EXPOSURE_FLASH;
    else if (a <= 16u) lab->exposure = VFX_EXPOSURE_DARK;
    else if (a <= 70u) lab->exposure = VFX_EXPOSURE_DIM;

    if (a <= 14u) {
        lab->ring_active = 1u;
        lab->ring_radius = (uint8_t)(1u + (a >> 1));
        lab->ring_phase = lab->phase3;
    } else lab->ring_active = 0u;

    /* State remains available for the isolated raster experiment, but the main
       validation ROM does not install repeated line interrupts. */
    if (a <= 8u) {
        lab->raster_warp_active = 1u;
        lab->raster_warp_center_y = (uint8_t)lab->ring_y;
        lab->raster_warp_radius = (uint8_t)(8u + (a << 1));
        lab->raster_warp_strength = (int8_t)(5 - (a >> 1));
    }

    /* Perceptual hit-stop doubles as CPU scheduling headroom. */
    if (a <= 14u) lab->ai_tick_div_hint = 0u;
    else if (a <= 45u) lab->ai_tick_div_hint = 4u;
    else if (a <= 90u) lab->ai_tick_div_hint = 2u;

    ++lab->grenade_age;
    if (lab->grenade_age > 150u) lab->grenade_age = 0u;
}

static void update_flashbang(VfxLab *lab) {
    uint8_t a = lab->flashbang_age;
    if (!a) return;
    if (a <= 3u) {
        lab->exposure = VFX_EXPOSURE_FLASH;
        if (a == 2u) { lab->blackout = 1u; lab->blackout_white = 1u; }
    } else if (a <= 34u) lab->exposure = VFX_EXPOSURE_DARK;
    else if (a <= 100u) lab->exposure = VFX_EXPOSURE_DIM;

    if (a <= 10u) lab->ai_tick_div_hint = 0u;
    else if (a <= 45u) lab->ai_tick_div_hint = 4u;
    else if (a <= 90u) lab->ai_tick_div_hint = 2u;

    ++lab->flashbang_age;
    if (lab->flashbang_age > 145u) lab->flashbang_age = 0u;
}

static void reset_frame_outputs(VfxLab *lab) {
    lab->exposure = VFX_EXPOSURE_NORMAL;
    lab->blackout = 0u;
    lab->blackout_white = 0u;
    lab->raster_warp_active = 0u;
    lab->raster_warp_strength = 0;
    lab->ai_tick_div_hint = 1u;
    lab->work_units_hint = 2u;
    lab->vram_budget_hint = 96u;
}

static void script_scene_events(VfxLab *lab) {
    uint16_t f = lab->scene_frame;
    if (lab->scene == VFX_SCENE_BULLETS) {
        if (f == 15u || f == 75u || f == 135u || f == 195u) {
            if ((f / 60u) & 1u)
                spawn_projectile(lab, cell_px(31u), cell_px(3u), cell_px(22u), cell_px(11u), 0u);
            else
                spawn_projectile(lab, cell_px(22u), cell_px(11u), cell_px(31u), cell_px(3u), 0u);
        }
    } else if (lab->scene == VFX_SCENE_BEAM) {
        if (f == 36u) vfx_lab_fire_beam(lab, cell_px(22u), cell_px(11u), cell_px(31u), cell_px(3u));
    } else if (lab->scene == VFX_SCENE_GRENADE) {
        if (f == 42u) vfx_lab_detonate_grenade(lab, cell_px(37u), cell_px(18u), -4, 2);
    } else if (lab->scene == VFX_SCENE_FLASHBANG) {
        if (f == 42u) vfx_lab_trigger_flashbang(lab, cell_px(27u), cell_px(8u));
    } else {
        if (f == 22u || f == 92u || f == 182u || f == 272u)
            spawn_projectile(lab, cell_px(22u), cell_px(11u), cell_px(31u), cell_px(3u),
                             (uint8_t)((f >> 5) & 1u));
        if (f == 58u || f == 230u)
            vfx_lab_fire_beam(lab, cell_px(22u), cell_px(11u), cell_px(31u), cell_px(3u));
        if (f == 138u)
            vfx_lab_detonate_grenade(lab, cell_px(37u), cell_px(18u), -3, -2);
        if (f == 330u)
            vfx_lab_trigger_flashbang(lab, cell_px(27u), cell_px(8u));
    }
}

void vfx_lab_tick(VfxLab *lab, Sim *sim) {
    reset_frame_outputs(lab);
    script_scene_events(lab);
    update_projectiles(lab);
    update_beam(lab);
    update_debris(lab, sim);
    update_shake(lab);
    update_flicker(lab);
    update_grenade(lab);
    update_flashbang(lab);
    if (lab->impact_active) --lab->impact_active;

    if (lab->beam.phase != VFX_BEAM_OFF && lab->exposure == VFX_EXPOSURE_NORMAL)
        lab->exposure = VFX_EXPOSURE_DIM;

    if (lab->blackout) {
        lab->ai_tick_div_hint = 0u;
        lab->work_units_hint = 8u;
        lab->vram_budget_hint = 768u;
    } else if (lab->grenade_age && lab->grenade_age <= 28u) {
        lab->work_units_hint = 8u;
        lab->vram_budget_hint = 320u;
    } else if (lab->beam.phase == VFX_BEAM_FORMING || lab->ring_active) {
        lab->work_units_hint = 4u;
        lab->vram_budget_hint = 160u;
    }

    ++lab->scene_frame;
    ++lab->total_frame;
    ++lab->phase3;
    if (lab->phase3 == 3u) lab->phase3 = 0u;
    if (lab->scene_frame >= scene_duration(lab->scene)) vfx_lab_next_scene(lab);
}

static void set_pose(Sim *sim, uint8_t id, uint8_t x, uint8_t y, uint8_t hit, uint8_t suppressed) {
    Agent *a = &sim->agents[id];
    a->x = x; a->y = y;
    a->home_x = x; a->home_y = y;
    a->alive = 1u;
    a->hp = SIM_MAX_HP;
    a->ammo = SIM_MAX_AMMO;
    a->hit_flash = hit;
    a->suppressed = suppressed;
}

void vfx_lab_apply_demo_agents(VfxLab *lab, Sim *sim) {
    uint8_t i;
    for (i = 0u; i < SIM_MAX_AGENTS; ++i) {
        sim->agents[i].alive = 0u;
        sim->agents[i].hit_flash = 0u;
        sim->agents[i].suppressed = 0u;
    }
    sim->agent_count = 6u;
    sim->red_count = 2u;
    sim->done = 0u;

    if (lab->scene == VFX_SCENE_GRENADE) {
        set_pose(sim, 0u, 32u, 16u, 0u, 0u);
        set_pose(sim, 4u, 40u, 16u, lab->grenade_age > 0u && lab->grenade_age < 8u, 0u);
        set_pose(sim, 5u, 35u, 20u, 0u, lab->grenade_age > 0u && lab->grenade_age < 40u);
    } else if (lab->scene == VFX_SCENE_FLASHBANG) {
        set_pose(sim, 0u, 23u, 11u, 0u, 0u);
        set_pose(sim, 4u, 31u, 7u, 0u, lab->flashbang_age > 0u && lab->flashbang_age < 90u);
        set_pose(sim, 5u, 27u, 4u, 0u, lab->flashbang_age > 0u && lab->flashbang_age < 90u);
    } else {
        set_pose(sim, 0u, 22u, 11u, 0u, 0u);
        set_pose(sim, 4u, 31u, 3u, lab->impact_active ? 1u : 0u, 0u);
        set_pose(sim, 5u, 28u, 10u, 0u, lab->beam.phase != VFX_BEAM_OFF ? 1u : 0u);
    }
}

uint8_t vfx_lab_beam_cell_active(const VfxLab *lab, uint8_t index) {
    const VfxBeam *b = &lab->beam;
    if (index >= b->cell_count || b->phase == VFX_BEAM_OFF) return 0u;
    if (b->phase == VFX_BEAM_FORMING) return index >= (uint8_t)(b->cell_count - b->formed_count);
    if (b->phase == VFX_BEAM_HOLD) return 1u;
    return index < (uint8_t)(b->cell_count - b->decay_count);
}

static uint8_t mod3_small(uint8_t v) {
    while (v >= 3u) v = (uint8_t)(v - 3u);
    return v;
}

uint8_t vfx_lab_beam_cell_visible_this_frame(const VfxLab *lab, uint8_t index) {
    uint8_t bucket;
    const VfxBeam *b = &lab->beam;
    if (!vfx_lab_beam_cell_active(lab, index)) return 0u;
    if (b->duty_numer >= 3u) return 1u;
    bucket = mod3_small((uint8_t)(mod3_small(index) + mod3_small((uint8_t)(b->cells[index].by << 1)) + lab->phase3));
    return bucket < b->duty_numer;
}

uint8_t vfx_lab_active_debris(const VfxLab *lab) {
    uint8_t i, n = 0u;
    for (i = 0u; i < FX_MAX_DEBRIS; ++i) if (lab->debris[i].active) ++n;
    return n;
}

const char *vfx_scene_name(uint8_t scene) {
    if (scene == VFX_SCENE_BULLETS) return "BULLET";
    if (scene == VFX_SCENE_BEAM) return "BEAM";
    if (scene == VFX_SCENE_GRENADE) return "GRENADE";
    if (scene == VFX_SCENE_FLASHBANG) return "FLASH";
    if (scene == VFX_SCENE_STRESS) return "STRESS";
    return "?";
}

void vfx_lab_target_init(VfxLab *lab, uint16_t seed) BANKED {
    vfx_lab_init(lab, seed);
}

void vfx_lab_target_step(VfxLab *lab, Sim *sim) BANKED {
    vfx_lab_tick(lab, sim);
    vfx_lab_apply_demo_agents(lab, sim);
}

void vfx_lab_target_set_scene(VfxLab *lab, uint8_t scene) BANKED {
    vfx_lab_set_scene(lab, scene);
}
