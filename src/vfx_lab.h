#ifndef GG_CQB_VFX_LAB_H
#define GG_CQB_VFX_LAB_H

#include <stdint.h>
#include "fx.h"
#include "sim.h"

#if defined(__SDCC)
#include <gbdk/platform.h>
#else
#ifndef BANKED
#define BANKED
#endif
#endif

#define VFX_BEAM_MAX_CELLS 40u
#define VFX_PROJECTILE_MAX 2u
#define VFX_DEBRIS_RENDER_MAX 8u
#define VFX_AI_IS_HELD(lab_ptr) ((lab_ptr)->ai_tick_div_hint == 0u)
#define VFX_AI_TICK_DUE(lab_ptr) ((lab_ptr)->ai_tick_div_hint != 0u && (((lab_ptr)->ai_tick_div_hint == 1u) || ((((lab_ptr)->total_frame) & ((lab_ptr)->ai_tick_div_hint - 1u)) == 0u)))

typedef enum {
    VFX_SCENE_BULLETS = 0,
    VFX_SCENE_BEAM,
    VFX_SCENE_GRENADE,
    VFX_SCENE_FLASHBANG,
    VFX_SCENE_STRESS,
    VFX_SCENE_COUNT
} VfxScene;

typedef enum {
    VFX_EXPOSURE_NORMAL = 0,
    VFX_EXPOSURE_DIM,
    VFX_EXPOSURE_DARK,
    VFX_EXPOSURE_FLASH
} VfxExposure;

typedef enum {
    VFX_BEAM_OFF = 0,
    VFX_BEAM_FORMING,
    VFX_BEAM_HOLD,
    VFX_BEAM_DECAY
} VfxBeamPhase;

typedef struct {
    uint8_t bx, by;
    uint8_t lx0, ly0;
    uint8_t lx1, ly1;
} VfxBeamCell;

typedef struct {
    uint8_t phase;
    uint8_t cell_count;
    uint8_t formed_count;
    uint8_t decay_count;
    uint8_t hold_frames;
    uint8_t phase_tick;
    uint8_t duty_numer;
    int16_t x0, y0;
    int16_t x1, y1;
    VfxBeamCell cells[VFX_BEAM_MAX_CELLS];
} VfxBeam;

typedef struct {
    uint8_t scene;
    uint16_t scene_frame;
    uint16_t total_frame;
    uint16_t rng;
    uint8_t phase3;

    FxProjectile projectiles[VFX_PROJECTILE_MAX];
    VfxBeam beam;
    FxDebris debris[FX_MAX_DEBRIS];

    int16_t impact_x, impact_y;
    uint8_t impact_active;

    int16_t ring_x, ring_y;
    uint8_t ring_active;
    uint8_t ring_radius;
    uint8_t ring_phase;

    int8_t shake_x, shake_y;
    int8_t shake_impulse_x, shake_impulse_y;
    uint8_t shake_age;
    uint8_t shake_duration;

    uint8_t grenade_age;
    uint8_t flashbang_age;
    uint8_t flicker_age;
    uint8_t flicker_duration;
    uint8_t blackout_run;

    uint8_t exposure;
    uint8_t blackout;
    uint8_t blackout_white;
    uint8_t raster_warp_active;
    uint8_t raster_warp_center_y;
    uint8_t raster_warp_radius;
    int8_t raster_warp_strength;

    /* AI scheduling affordance for integration with the real sim:
       0 = hold AI, 1 = normal, 2 = half-rate, 4 = quarter-rate. */
    uint8_t ai_tick_div_hint;
    uint8_t work_units_hint;
    uint16_t vram_budget_hint;
} VfxLab;

/* Near/local API for host tests and calls within target bank 1. */
void vfx_lab_init(VfxLab *lab, uint16_t seed);
void vfx_lab_set_scene(VfxLab *lab, uint8_t scene);
void vfx_lab_next_scene(VfxLab *lab);
void vfx_lab_tick(VfxLab *lab, Sim *sim);
void vfx_lab_apply_demo_agents(VfxLab *lab, Sim *sim);
void vfx_lab_fire_beam(VfxLab *lab, int16_t source_x, int16_t source_y,
                       int16_t hit_x, int16_t hit_y);
void vfx_lab_detonate_grenade(VfxLab *lab, int16_t x, int16_t y,
                              int8_t impulse_x, int8_t impulse_y);
void vfx_lab_trigger_flashbang(VfxLab *lab, int16_t x, int16_t y);
uint8_t vfx_lab_beam_cell_active(const VfxLab *lab, uint8_t index);
uint8_t vfx_lab_beam_cell_visible_this_frame(const VfxLab *lab, uint8_t index);
uint8_t vfx_lab_active_debris(const VfxLab *lab);
const char *vfx_scene_name(uint8_t scene);

/* Fixed-bank entry points. These are the only VFX-lab calls target main should
   make across the cartridge-bank boundary. */
void vfx_lab_target_init(VfxLab *lab, uint16_t seed) BANKED;
void vfx_lab_target_step(VfxLab *lab, Sim *sim) BANKED;
void vfx_lab_target_set_scene(VfxLab *lab, uint8_t scene) BANKED;

#endif
