#ifndef GG_CQB_GG_VFX_H
#define GG_CQB_GG_VFX_H

#include <stdint.h>
#include "vfx_lab.h"
#include "vfx_queue.h"
#include "gg_world.h"

#if defined(__SDCC)
#include <gbdk/platform.h>
#else
#ifndef BANKED
#define BANKED
#endif
#endif

#define GG_VFX_SCRATCH_BASE 64u
#define GG_VFX_SCRATCH_COUNT 32u
#define GG_VFX_SPRITE_INDEX_BASE 16u
#define GG_VFX_SPRITE_INDEX_END 40u
#define GG_VFX_RASTER_BANDS 24u
#define GG_VFX_GLOW_DIRTY_CAP 32u

typedef struct {
    VfxQueue queue;
    uint8_t scratch_used[GG_VFX_SCRATCH_COUNT];
    uint8_t beam_slot[VFX_BEAM_MAX_CELLS];
    uint8_t beam_map_shown[VFX_BEAM_MAX_CELLS];
    uint8_t beam_bx[VFX_BEAM_MAX_CELLS];
    uint8_t beam_by[VFX_BEAM_MAX_CELLS];
    uint8_t glow_ref[GG_WORLD_BLOCK_W * GG_WORLD_BLOCK_H];
    uint8_t glow_shown[GG_WORLD_BLOCK_W * GG_WORLD_BLOCK_H];
    uint8_t glow_dirty_bx[GG_VFX_GLOW_DIRTY_CAP];
    uint8_t glow_dirty_by[GG_VFX_GLOW_DIRTY_CAP];
    uint8_t glow_dirty_count;

    uint8_t projectile_cache_valid[VFX_PROJECTILE_MAX];
    int16_t projectile_cache_vx[VFX_PROJECTILE_MAX];
    int16_t projectile_cache_vy[VFX_PROJECTILE_MAX];
    uint8_t projectile_cache_seed[VFX_PROJECTILE_MAX];

    uint8_t ring_cache_radius;
    uint8_t ring_cache_valid;
    uint8_t last_exposure;
    uint8_t last_blackout;
    uint8_t scratch_used_count;
    uint8_t fx_sprites_visible;
    uint8_t queue_high_water;
    uint16_t vram_bytes_last;
    uint16_t vram_bytes_total;
    int8_t raster_offsets[GG_VFX_RASTER_BANDS];
} GgVfx;

/* Near/local bank-3 API. */
void gg_vfx_init(GgVfx *fx);
void gg_vfx_reset(GgVfx *fx, const Sim *sim);
void gg_vfx_sync(GgVfx *fx, const VfxLab *lab, const Sim *sim);
void gg_vfx_flush(GgVfx *fx, uint16_t budget_bytes);
void gg_vfx_render_sprites(GgVfx *fx, const VfxLab *lab, uint8_t camera_x);
void gg_vfx_build_raster_offsets(GgVfx *fx, const VfxLab *lab);

/* Fixed-bank target entry points. Keep crossings coarse: one init call and one
   render/transfer call per video frame. */
void gg_vfx_target_init(GgVfx *fx, const Sim *sim) BANKED;
void gg_vfx_target_frame(GgVfx *fx, const VfxLab *lab, const Sim *sim,
                         uint8_t camera_x) BANKED;

#endif
