#ifndef GG_CQB_VFX_QUEUE_H
#define GG_CQB_VFX_QUEUE_H

#include <stdint.h>

#define VFX_QUEUE_CAP 10u
#define VFX_TILE_BYTES 32u

typedef enum {
    VFX_JOB_NONE = 0,
    VFX_JOB_BG_TILE,
    VFX_JOB_SPRITE_TILE,
    VFX_JOB_BG_CELL
} VfxJobKind;

typedef struct {
    uint8_t kind;
    uint8_t priority;
    uint16_t index;
    uint8_t x;
    uint8_t y;
    uint16_t attr;
    uint8_t data[VFX_TILE_BYTES];
} VfxJob;

typedef struct {
    VfxJob jobs[VFX_QUEUE_CAP];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t dropped;
    uint16_t bytes_queued;
    uint16_t bytes_flushed_last;
    uint16_t bytes_flushed_total;
} VfxQueue;

void vfx_queue_init(VfxQueue *q);
uint8_t vfx_queue_push_bg_tile(VfxQueue *q, uint16_t tile, const uint8_t *data, uint8_t priority);
uint8_t vfx_queue_push_sprite_tile(VfxQueue *q, uint8_t tile, const uint8_t *data, uint8_t priority);
uint8_t vfx_queue_push_bg_cell(VfxQueue *q, uint8_t x, uint8_t y, uint16_t attr, uint8_t priority);
const VfxJob *vfx_queue_peek(const VfxQueue *q);
void vfx_queue_pop(VfxQueue *q, uint16_t charged_bytes);
uint16_t vfx_job_cost(const VfxJob *job);

#endif
