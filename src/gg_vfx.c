#if defined(__SDCC)
#pragma bank 3
#endif

#include <gbdk/platform.h>
#include "gg_vfx.h"
#include "gg_world.h"
#include "fx.h"

#define VFX_PAL1_ATTR ((uint16_t)S_PAL(1u) << 8)
#define VFX_SPR_TRACER_BASE 4u
#define VFX_SPR_IMPACT 10u
#define VFX_SPR_RING_BASE 11u
#define VFX_SPR_DEBRIS_SMALL 15u
#define VFX_SPR_DEBRIS_CHUNK 16u
#define VFX_SIDX_PROJECTILE0 16u
#define VFX_SIDX_IMPACT 18u
#define VFX_SIDX_RING0 19u
#define VFX_SIDX_DEBRIS_BASE 23u

static const uint16_t k_block_row[GG_WORLD_BLOCK_H] = {
    0u,23u,46u,69u,92u,115u,138u,161u,184u,207u,230u,253u
};

static const palette_color_t k_bg_palettes[4][16] = {
    { RGB(0,0,0), RGB(1,2,4), RGB(7,8,9), RGB(15,9,1), RGB(6,4,1), RGB(1,12,15), RGB(15,2,2), RGB(15,15,15), RGB(15,15,1), RGB(0,5,8), RGB(7,0,0), RGB(10,10,10), RGB(5,15,5), RGB(11,1,12), RGB(2,2,2), RGB(15,15,15) },
    { RGB(0,0,0), RGB(0,1,2), RGB(3,4,5), RGB(7,4,0), RGB(3,2,0), RGB(0,6,8), RGB(8,1,1), RGB(15,15,15), RGB(15,15,2), RGB(0,3,5), RGB(4,0,0), RGB(7,7,7), RGB(3,10,3), RGB(6,0,7), RGB(1,1,1), RGB(15,15,15) },
    { RGB(0,0,0), RGB(0,0,1), RGB(1,1,2), RGB(3,1,0), RGB(1,1,0), RGB(0,3,4), RGB(4,0,0), RGB(15,15,15), RGB(15,15,3), RGB(0,1,2), RGB(2,0,0), RGB(4,4,4), RGB(2,7,2), RGB(3,0,4), RGB(0,0,0), RGB(15,15,15) },
    { RGB(15,15,15), RGB(15,15,15), RGB(15,15,15), RGB(15,14,10), RGB(15,14,10), RGB(12,15,15), RGB(15,12,12), RGB(15,15,15), RGB(15,15,15), RGB(12,15,15), RGB(15,10,10), RGB(15,15,15), RGB(14,15,14), RGB(15,12,15), RGB(13,13,13), RGB(15,15,15) }
};

static const palette_color_t k_lit_palettes[4][16] = {
    { RGB(0,0,0), RGB(4,7,11), RGB(12,13,15), RGB(15,12,4), RGB(10,7,3), RGB(1,12,15), RGB(15,2,2), RGB(15,15,15), RGB(15,15,3), RGB(0,7,12), RGB(9,0,0), RGB(12,12,12), RGB(7,15,7), RGB(13,2,15), RGB(4,4,4), RGB(15,15,15) },
    { RGB(0,0,0), RGB(3,5,8), RGB(9,10,12), RGB(14,10,3), RGB(8,5,2), RGB(0,8,11), RGB(11,1,1), RGB(15,15,15), RGB(15,15,3), RGB(0,5,9), RGB(6,0,0), RGB(9,9,9), RGB(5,13,5), RGB(9,1,11), RGB(2,2,2), RGB(15,15,15) },
    { RGB(0,0,0), RGB(2,3,5), RGB(6,7,9), RGB(10,7,2), RGB(5,3,1), RGB(0,5,7), RGB(7,0,0), RGB(15,15,15), RGB(15,15,4), RGB(0,3,6), RGB(4,0,0), RGB(6,6,6), RGB(4,10,4), RGB(6,0,8), RGB(1,1,1), RGB(15,15,15) },
    { RGB(15,15,15), RGB(15,15,15), RGB(15,15,15), RGB(15,15,12), RGB(15,15,12), RGB(14,15,15), RGB(15,14,14), RGB(15,15,15), RGB(15,15,15), RGB(14,15,15), RGB(15,13,13), RGB(15,15,15), RGB(15,15,15), RGB(15,14,15), RGB(14,14,14), RGB(15,15,15) }
};

static uint16_t block_index(uint8_t bx,uint8_t by){return (uint16_t)(k_block_row[by]+bx);}
static uint16_t base_attr(const Sim *sim,uint8_t bx,uint8_t by,uint8_t lit){uint16_t tile=gg_world_pattern_for_block(sim,bx,by);return lit?(uint16_t)(tile|VFX_PAL1_ATTR):tile;}

static uint8_t beam_active_local(const VfxLab *lab,uint8_t index){
    const VfxBeam *b=&lab->beam;
    if(index>=b->cell_count||b->phase==VFX_BEAM_OFF)return 0u;
    if(b->phase==VFX_BEAM_FORMING)return index>=(uint8_t)(b->cell_count-b->formed_count);
    if(b->phase==VFX_BEAM_HOLD)return 1u;
    return index<(uint8_t)(b->cell_count-b->decay_count);
}
static uint8_t beam_visible_local(const VfxLab *lab,uint8_t index){
    const VfxBeam *b=&lab->beam;
    if(!beam_active_local(lab,index))return 0u;
    if(b->duty_numer>=3u)return 1u;
    return fx_dither_on(index,b->cells[index].by,lab->phase3,b->duty_numer);
}
static uint8_t active_debris_local(const VfxLab *lab){uint8_t i,n=0u;for(i=0u;i<FX_MAX_DEBRIS;++i)if(lab->debris[i].active)++n;return n;}

static uint8_t cell_has_beam(const GgVfx *fx,uint8_t bx,uint8_t by){uint8_t i;for(i=0u;i<VFX_BEAM_MAX_CELLS;++i)if(fx->beam_slot[i]!=0xffu&&fx->beam_bx[i]==bx&&fx->beam_by[i]==by)return 1u;return 0u;}
static void mark_glow_dirty(GgVfx *fx,uint8_t bx,uint8_t by){uint8_t i;for(i=0u;i<fx->glow_dirty_count;++i)if(fx->glow_dirty_bx[i]==bx&&fx->glow_dirty_by[i]==by)return;if(fx->glow_dirty_count<GG_VFX_GLOW_DIRTY_CAP){fx->glow_dirty_bx[fx->glow_dirty_count]=bx;fx->glow_dirty_by[fx->glow_dirty_count]=by;++fx->glow_dirty_count;}}
static void glow_change(GgVfx *fx,int8_t bx,int8_t by,int8_t delta){uint16_t idx;uint8_t before;if(bx<0||by<0||bx>=(int8_t)GG_WORLD_BLOCK_W||by>=(int8_t)GG_WORLD_BLOCK_H)return;idx=block_index((uint8_t)bx,(uint8_t)by);before=fx->glow_ref[idx];if(delta>0){if(fx->glow_ref[idx]<255u)++fx->glow_ref[idx];}else if(fx->glow_ref[idx])--fx->glow_ref[idx];if((before==0u)!=(fx->glow_ref[idx]==0u))mark_glow_dirty(fx,(uint8_t)bx,(uint8_t)by);}
static void glow_for_beam_cell(GgVfx *fx,uint8_t bx,uint8_t by,int8_t delta){glow_change(fx,(int8_t)bx-1,(int8_t)by,delta);glow_change(fx,(int8_t)bx+1,(int8_t)by,delta);glow_change(fx,(int8_t)bx,(int8_t)by-1,delta);glow_change(fx,(int8_t)bx,(int8_t)by+1,delta);}
static uint8_t alloc_scratch(GgVfx *fx){uint8_t i;for(i=0u;i<GG_VFX_SCRATCH_COUNT;++i)if(!fx->scratch_used[i]){fx->scratch_used[i]=1u;++fx->scratch_used_count;return i;}return 0xffu;}
static void free_scratch(GgVfx *fx,uint8_t slot){if(slot>=GG_VFX_SCRATCH_COUNT||!fx->scratch_used[slot])return;fx->scratch_used[slot]=0u;if(fx->scratch_used_count)--fx->scratch_used_count;}

static void make_impact_tile(uint8_t *tile){fx_tile_clear(tile);fx_tile_set_pixel(tile,4u,1u,7u);fx_tile_set_pixel(tile,4u,2u,15u);fx_tile_set_pixel(tile,1u,4u,7u);fx_tile_set_pixel(tile,2u,4u,15u);fx_tile_set_pixel(tile,3u,4u,15u);fx_tile_set_pixel(tile,4u,4u,15u);fx_tile_set_pixel(tile,5u,4u,15u);fx_tile_set_pixel(tile,6u,4u,15u);fx_tile_set_pixel(tile,4u,5u,15u);fx_tile_set_pixel(tile,4u,6u,7u);}
static void make_debris_tile(uint8_t *tile,uint8_t chunk){fx_tile_clear(tile);fx_tile_set_pixel(tile,3u,3u,chunk?12u:8u);fx_tile_set_pixel(tile,4u,3u,chunk?12u:8u);if(chunk){fx_tile_set_pixel(tile,3u,4u,12u);fx_tile_set_pixel(tile,4u,4u,7u);}}

void gg_vfx_init(GgVfx *fx){
    uint16_t wi;uint8_t i,tile[32u];vfx_queue_init(&fx->queue);
    for(i=0u;i<GG_VFX_SCRATCH_COUNT;++i)fx->scratch_used[i]=0u;
    for(i=0u;i<VFX_BEAM_MAX_CELLS;++i){fx->beam_slot[i]=0xffu;fx->beam_map_shown[i]=0u;fx->beam_bx[i]=fx->beam_by[i]=0u;}
    for(wi=0u;wi<(uint16_t)(GG_WORLD_BLOCK_W*GG_WORLD_BLOCK_H);++wi)fx->glow_ref[wi]=fx->glow_shown[wi]=0u;
    fx->glow_dirty_count=0u;for(i=0u;i<VFX_PROJECTILE_MAX;++i)fx->projectile_cache_valid[i]=0u;
    fx->ring_cache_valid=0u;fx->last_exposure=0xffu;fx->last_blackout=0u;fx->scratch_used_count=0u;fx->fx_sprites_visible=0u;fx->queue_high_water=0u;fx->vram_bytes_last=fx->vram_bytes_total=0u;
    for(i=0u;i<GG_VFX_RASTER_BANDS;++i)fx->raster_offsets[i]=0;
    make_impact_tile(tile);set_sprite_4bpp_data(VFX_SPR_IMPACT,1u,tile);
    make_debris_tile(tile,0u);set_sprite_4bpp_data(VFX_SPR_DEBRIS_SMALL,1u,tile);
    make_debris_tile(tile,1u);set_sprite_4bpp_data(VFX_SPR_DEBRIS_CHUNK,1u,tile);
}

void gg_vfx_reset(GgVfx *fx,const Sim *sim){uint16_t wi;uint8_t i;vfx_queue_init(&fx->queue);for(i=0u;i<GG_VFX_SCRATCH_COUNT;++i)fx->scratch_used[i]=0u;for(i=0u;i<VFX_BEAM_MAX_CELLS;++i){fx->beam_slot[i]=0xffu;fx->beam_map_shown[i]=0u;}for(wi=0u;wi<(uint16_t)(GG_WORLD_BLOCK_W*GG_WORLD_BLOCK_H);++wi)fx->glow_ref[wi]=fx->glow_shown[wi]=0u;fx->glow_dirty_count=0u;fx->scratch_used_count=0u;fx->ring_cache_valid=0u;for(i=0u;i<VFX_PROJECTILE_MAX;++i)fx->projectile_cache_valid[i]=0u;gg_world_upload_all(sim);}

static void apply_exposure(GgVfx *fx,uint8_t exposure){if(exposure>VFX_EXPOSURE_FLASH)exposure=VFX_EXPOSURE_NORMAL;if(fx->last_exposure==exposure)return;set_bkg_palette(0u,1u,k_bg_palettes[exposure]);set_sprite_palette(0u,1u,k_lit_palettes[exposure]);fx->last_exposure=exposure;fx->vram_bytes_last=(uint16_t)(fx->vram_bytes_last+64u);fx->vram_bytes_total=(uint16_t)(fx->vram_bytes_total+64u);}

static uint8_t activate_beam_cell(GgVfx *fx,const VfxLab *lab,const Sim *sim,uint8_t i){const VfxBeamCell *c=&lab->beam.cells[i];uint8_t slot,tile[32u];uint16_t scratch_tile;if(fx->beam_slot[i]!=0xffu)return 1u;if(fx->queue.count>(VFX_QUEUE_CAP-2u))return 0u;slot=alloc_scratch(fx);if(slot==0xffu)return 0u;scratch_tile=(uint16_t)(GG_VFX_SCRATCH_BASE+slot);gg_world_make_block_tile(sim,c->bx,c->by,tile);fx_tile_draw_line(tile,(int8_t)c->lx0,(int8_t)c->ly0,(int8_t)c->lx1,(int8_t)c->ly1,8u);if(!vfx_queue_push_bg_tile(&fx->queue,scratch_tile,tile,3u)||!vfx_queue_push_bg_cell(&fx->queue,c->bx,(uint8_t)(GG_WORLD_TILE_Y+c->by),(uint16_t)(scratch_tile|VFX_PAL1_ATTR),3u)){free_scratch(fx,slot);return 0u;}fx->beam_slot[i]=slot;fx->beam_map_shown[i]=1u;fx->beam_bx[i]=c->bx;fx->beam_by[i]=c->by;glow_for_beam_cell(fx,c->bx,c->by,1);return 1u;}

static uint8_t deactivate_beam_cell(GgVfx *fx,const Sim *sim,uint8_t i){uint8_t slot=fx->beam_slot[i],bx,by;uint16_t idx;if(slot==0xffu)return 1u;if(fx->queue.count>=VFX_QUEUE_CAP)return 0u;bx=fx->beam_bx[i];by=fx->beam_by[i];idx=block_index(bx,by);if(!vfx_queue_push_bg_cell(&fx->queue,bx,(uint8_t)(GG_WORLD_TILE_Y+by),base_attr(sim,bx,by,fx->glow_ref[idx]!=0u),3u))return 0u;/* mutate ownership only after the restore job is safely queued */glow_for_beam_cell(fx,bx,by,-1);fx->beam_slot[i]=0xffu;fx->beam_map_shown[i]=0u;free_scratch(fx,slot);return 1u;}

static uint8_t sync_beam_dither(GgVfx *fx,const VfxLab *lab,const Sim *sim,uint8_t i){uint8_t want,bx,by;uint16_t attr,idx;if(fx->beam_slot[i]==0xffu)return 1u;want=beam_visible_local(lab,i);if(fx->beam_map_shown[i]==want)return 1u;if(fx->queue.count>=VFX_QUEUE_CAP)return 0u;bx=fx->beam_bx[i];by=fx->beam_by[i];idx=block_index(bx,by);attr=want?(uint16_t)((GG_VFX_SCRATCH_BASE+fx->beam_slot[i])|VFX_PAL1_ATTR):base_attr(sim,bx,by,fx->glow_ref[idx]!=0u);if(!vfx_queue_push_bg_cell(&fx->queue,bx,(uint8_t)(GG_WORLD_TILE_Y+by),attr,2u))return 0u;fx->beam_map_shown[i]=want;return 1u;}

static void sync_beam(GgVfx *fx,const VfxLab *lab,const Sim *sim,uint8_t units){uint8_t i;for(i=0u;i<VFX_BEAM_MAX_CELLS&&units;++i){uint8_t active=beam_active_local(lab,i);if(active&&fx->beam_slot[i]==0xffu){if(!activate_beam_cell(fx,lab,sim,i))break;--units;}else if(!active&&fx->beam_slot[i]!=0xffu){if(!deactivate_beam_cell(fx,sim,i))break;--units;}else if(active&&fx->beam_slot[i]!=0xffu&&fx->beam_map_shown[i]!=beam_visible_local(lab,i)){if(!sync_beam_dither(fx,lab,sim,i))break;--units;}}}

static void sync_glow_dirty(GgVfx *fx,const Sim *sim,uint8_t units){while(fx->glow_dirty_count&&units&&fx->queue.count<VFX_QUEUE_CAP){uint8_t pos=(uint8_t)(fx->glow_dirty_count-1u);uint8_t bx=fx->glow_dirty_bx[pos],by=fx->glow_dirty_by[pos];uint16_t idx=block_index(bx,by);uint8_t want=fx->glow_ref[idx]!=0u;if(!cell_has_beam(fx,bx,by)&&fx->glow_shown[idx]!=want){if(!vfx_queue_push_bg_cell(&fx->queue,bx,(uint8_t)(GG_WORLD_TILE_Y+by),base_attr(sim,bx,by,want),1u))break;fx->glow_shown[idx]=want;--units;}--fx->glow_dirty_count;}}

static void sync_projectile_tiles(GgVfx *fx,const VfxLab *lab){uint8_t i;for(i=0u;i<VFX_PROJECTILE_MAX;++i){const FxProjectile *p=&lab->projectiles[i];if(!p->active){fx->projectile_cache_valid[i]=0u;continue;}if(!fx->projectile_cache_valid[i]||fx->projectile_cache_vx[i]!=p->vx||fx->projectile_cache_vy[i]!=p->vy||fx->projectile_cache_seed[i]!=p->seed){uint8_t phase;if(fx->queue.count>(VFX_QUEUE_CAP-3u))continue;for(phase=0u;phase<3u;++phase){uint8_t tile[32u];int8_t dx=(int8_t)(p->vx>>FX_FP_SHIFT),dy=(int8_t)(p->vy>>FX_FP_SHIFT);if(!dx&&p->vx)dx=p->vx>0?1:-1;if(!dy&&p->vy)dy=p->vy>0?1:-1;fx_tile_draw_tracer(tile,dx,dy,phase,p->seed,15u,8u);vfx_queue_push_sprite_tile(&fx->queue,(uint8_t)(VFX_SPR_TRACER_BASE+i*3u+phase),tile,3u);}fx->projectile_cache_vx[i]=p->vx;fx->projectile_cache_vy[i]=p->vy;fx->projectile_cache_seed[i]=p->seed;fx->projectile_cache_valid[i]=1u;}}}

static void sync_ring_tiles(GgVfx *fx,const VfxLab *lab){uint8_t q;if(!lab->ring_active){fx->ring_cache_valid=0u;return;}/* Do not regenerate four tiles merely because temporal dither phase changed. */if(fx->ring_cache_valid&&fx->ring_cache_radius==lab->ring_radius)return;if(fx->queue.count>(VFX_QUEUE_CAP-4u))return;for(q=0u;q<4u;++q){uint8_t tile[32u];fx_tile_draw_ring16_quadrant(tile,q,lab->ring_radius,12u,lab->ring_phase);vfx_queue_push_sprite_tile(&fx->queue,(uint8_t)(VFX_SPR_RING_BASE+q),tile,2u);}fx->ring_cache_radius=lab->ring_radius;fx->ring_cache_valid=1u;}

void gg_vfx_sync(GgVfx *fx,const VfxLab *lab,const Sim *sim){apply_exposure(fx,lab->exposure);sync_projectile_tiles(fx,lab);sync_ring_tiles(fx,lab);sync_beam(fx,lab,sim,lab->work_units_hint);sync_glow_dirty(fx,sim,lab->work_units_hint);if(fx->queue.count>fx->queue_high_water)fx->queue_high_water=fx->queue.count;}

void gg_vfx_flush(GgVfx *fx,uint16_t budget_bytes){const VfxJob *job;uint16_t before=fx->queue.bytes_flushed_last;while((job=vfx_queue_peek(&fx->queue))!=0){uint16_t cost=vfx_job_cost(job);if(cost>budget_bytes)break;if(job->kind==VFX_JOB_BG_TILE)set_bkg_4bpp_data(job->index,1u,job->data);else if(job->kind==VFX_JOB_SPRITE_TILE)set_sprite_4bpp_data((uint8_t)job->index,1u,job->data);else if(job->kind==VFX_JOB_BG_CELL)set_attributed_tile_xy(job->x,job->y,job->attr);budget_bytes=(uint16_t)(budget_bytes-cost);vfx_queue_pop(&fx->queue,cost);}{uint16_t delta=(uint16_t)(fx->queue.bytes_flushed_last-before);fx->vram_bytes_last=(uint16_t)(fx->vram_bytes_last+delta);fx->vram_bytes_total=(uint16_t)(fx->vram_bytes_total+delta);}}

static void hide_fx_sprites(void){uint8_t i;for(i=GG_VFX_SPRITE_INDEX_BASE;i<GG_VFX_SPRITE_INDEX_END;++i)hide_sprite(i);}
static uint8_t put_fx_sprite(uint8_t index,uint8_t tile,int16_t sx,int16_t sy){if(sx<-8||sx>167||sy<-8||sy>151){hide_sprite(index);return 0u;}set_sprite_tile(index,tile);move_sprite(index,(uint8_t)(DEVICE_SPRITE_PX_OFFSET_X+sx),(uint8_t)(DEVICE_SPRITE_PX_OFFSET_Y+sy));return 1u;}

void gg_vfx_render_sprites(GgVfx *fx,const VfxLab *lab,uint8_t camera_x){uint8_t i,debris_drawn=0u,debris_active=active_debris_local(lab);hide_fx_sprites();fx->fx_sprites_visible=0u;for(i=0u;i<VFX_PROJECTILE_MAX;++i){const FxProjectile *p=&lab->projectiles[i];if(p->active&&fx->projectile_cache_valid[i]){int16_t sx=(int16_t)(p->x>>FX_FP_SHIFT)-camera_x+lab->shake_x-4,sy=(int16_t)GG_WORLD_TILE_Y*8+(int16_t)(p->y>>FX_FP_SHIFT)+lab->shake_y-4;uint8_t tile=(uint8_t)(VFX_SPR_TRACER_BASE+i*3u+lab->phase3);fx->fx_sprites_visible+=put_fx_sprite((uint8_t)(VFX_SIDX_PROJECTILE0+i),tile,sx,sy);}}
    if(lab->impact_active){int16_t sx=lab->impact_x-camera_x+lab->shake_x-4,sy=(int16_t)GG_WORLD_TILE_Y*8+lab->impact_y+lab->shake_y-4;fx->fx_sprites_visible+=put_fx_sprite(VFX_SIDX_IMPACT,VFX_SPR_IMPACT,sx,sy);}
    if(lab->ring_active&&fx->ring_cache_valid){int16_t cx=lab->ring_x-camera_x+lab->shake_x,cy=(int16_t)GG_WORLD_TILE_Y*8+lab->ring_y+lab->shake_y;uint8_t q;for(q=0u;q<4u;++q){int16_t sx=(int16_t)(cx-7+((q&1u)?8:0)),sy=(int16_t)(cy-7+((q&2u)?8:0));if(lab->grenade_age>10u&&!fx_dither_on(q,0u,lab->phase3,2u)){hide_sprite((uint8_t)(VFX_SIDX_RING0+q));continue;}fx->fx_sprites_visible+=put_fx_sprite((uint8_t)(VFX_SIDX_RING0+q),(uint8_t)(VFX_SPR_RING_BASE+q),sx,sy);}}
    for(i=0u;i<FX_MAX_DEBRIS&&debris_drawn<VFX_DEBRIS_RENDER_MAX;++i){const FxDebris *p=&lab->debris[i];uint8_t duty=3u;if(!p->active)continue;if(((i+(uint8_t)lab->total_frame)&1u)&&debris_active>VFX_DEBRIS_RENDER_MAX)continue;if(p->settle_age>16u)duty=1u;else if(p->settle_age>6u)duty=2u;if(!fx_dither_on(i,p->seed,lab->phase3,duty))continue;{int16_t sx=(int16_t)(p->x>>FX_FP_SHIFT)-camera_x+lab->shake_x-4,sy=(int16_t)GG_WORLD_TILE_Y*8+(int16_t)(p->y>>FX_FP_SHIFT)+lab->shake_y-4;uint8_t tile=p->size_class==FX_DEBRIS_CHUNK?VFX_SPR_DEBRIS_CHUNK:VFX_SPR_DEBRIS_SMALL;fx->fx_sprites_visible+=put_fx_sprite((uint8_t)(VFX_SIDX_DEBRIS_BASE+debris_drawn),tile,sx,sy);++debris_drawn;}}
}

void gg_vfx_build_raster_offsets(GgVfx *fx,const VfxLab *lab){uint8_t i;int16_t center=(int16_t)(GG_WORLD_TILE_Y+DEVICE_SCREEN_Y_OFFSET)*8+lab->raster_warp_center_y;uint8_t half=(uint8_t)(lab->raster_warp_radius>>1),quarter=(uint8_t)(lab->raster_warp_radius>>2);for(i=0u;i<GG_VFX_RASTER_BANDS;++i){int16_t scan=(int16_t)i*8+4,dy=(int16_t)(scan-center),ady=dy<0?(int16_t)-dy:dy;int8_t off=0;if(lab->raster_warp_active&&ady<lab->raster_warp_radius){int8_t mag=1;if(ady<quarter)mag=lab->raster_warp_strength;else if(ady<half)mag=(int8_t)((lab->raster_warp_strength+1)>>1);off=dy<0?(int8_t)-mag:mag;}fx->raster_offsets[i]=off;}}

void gg_vfx_target_init(GgVfx *fx,const Sim *sim) BANKED {gg_world_reset_patterns();gg_vfx_init(fx);gg_world_upload_all(sim);}

void gg_vfx_target_frame(GgVfx *fx,const VfxLab *lab,const Sim *sim,uint8_t camera_x) BANKED {
    uint8_t passes=lab->blackout?3u:1u;
    uint16_t remaining=lab->vram_budget_hint;
    fx->vram_bytes_last=0u;
    fx->queue.bytes_flushed_last=0u;

    if(lab->blackout!=fx->last_blackout){
        if(lab->blackout){SET_BORDER_COLOR(lab->blackout_white?15u:0u);DISPLAY_OFF;}
        else {SET_BORDER_COLOR(0u);DISPLAY_ON;}
        fx->last_blackout=lab->blackout;
    } else if(lab->blackout) SET_BORDER_COLOR(lab->blackout_white?15u:0u);

    while(passes-- && remaining){
        uint16_t before=fx->queue.bytes_flushed_last;
        uint16_t used;
        gg_vfx_sync(fx,lab,sim);
        gg_vfx_flush(fx,remaining);
        used=(uint16_t)(fx->queue.bytes_flushed_last-before);
        if(used>=remaining){remaining=0u;break;}
        remaining=(uint16_t)(remaining-used);
        if(!used && !fx->queue.count) break;
    }
    gg_vfx_render_sprites(fx,lab,camera_x);
}
