#include "vfx_queue.h"

static void copy_tile(uint8_t *dst, const uint8_t *src) {
    uint8_t i;
    for (i = 0u; i < VFX_TILE_BYTES; ++i) dst[i] = src[i];
}

void vfx_queue_init(VfxQueue *q) {
    q->head = q->tail = q->count = q->dropped = 0u;
    q->bytes_queued = 0u;
    q->bytes_flushed_last = 0u;
    q->bytes_flushed_total = 0u;
}

static VfxJob *reserve_job(VfxQueue *q, uint8_t priority) {
    uint8_t i;
    if (q->count < VFX_QUEUE_CAP) {
        VfxJob *job = &q->jobs[q->tail];
        q->tail = (uint8_t)((q->tail + 1u) % VFX_QUEUE_CAP);
        ++q->count;
        job->priority = priority;
        return job;
    }

    /* If full, permit a high-priority job to replace the first lower-priority
       queued job. This is rare and deliberately O(N) because N is ten. */
    for (i = 0u; i < q->count; ++i) {
        uint8_t idx = (uint8_t)((q->head + i) % VFX_QUEUE_CAP);
        if (q->jobs[idx].priority < priority) {
            ++q->dropped;
            q->bytes_queued = (uint16_t)(q->bytes_queued - vfx_job_cost(&q->jobs[idx]));
            q->jobs[idx].priority = priority;
            return &q->jobs[idx];
        }
    }
    ++q->dropped;
    return 0;
}

uint8_t vfx_queue_push_bg_tile(VfxQueue *q, uint16_t tile, const uint8_t *data, uint8_t priority) {
    VfxJob *job = reserve_job(q, priority);
    if (!job) return 0u;
    job->kind = VFX_JOB_BG_TILE;
    job->index = tile;
    copy_tile(job->data, data);
    q->bytes_queued = (uint16_t)(q->bytes_queued + 32u);
    return 1u;
}

uint8_t vfx_queue_push_sprite_tile(VfxQueue *q, uint8_t tile, const uint8_t *data, uint8_t priority) {
    VfxJob *job = reserve_job(q, priority);
    if (!job) return 0u;
    job->kind = VFX_JOB_SPRITE_TILE;
    job->index = tile;
    copy_tile(job->data, data);
    q->bytes_queued = (uint16_t)(q->bytes_queued + 32u);
    return 1u;
}

uint8_t vfx_queue_push_bg_cell(VfxQueue *q, uint8_t x, uint8_t y, uint16_t attr, uint8_t priority) {
    VfxJob *job = reserve_job(q, priority);
    if (!job) return 0u;
    job->kind = VFX_JOB_BG_CELL;
    job->x = x;
    job->y = y;
    job->attr = attr;
    q->bytes_queued = (uint16_t)(q->bytes_queued + 2u);
    return 1u;
}

const VfxJob *vfx_queue_peek(const VfxQueue *q) {
    if (!q->count) return 0;
    return &q->jobs[q->head];
}

void vfx_queue_pop(VfxQueue *q, uint16_t charged_bytes) {
    if (!q->count) return;
    q->head = (uint8_t)((q->head + 1u) % VFX_QUEUE_CAP);
    --q->count;
    if (q->bytes_queued >= charged_bytes) q->bytes_queued = (uint16_t)(q->bytes_queued - charged_bytes);
    else q->bytes_queued = 0u;
    q->bytes_flushed_last = (uint16_t)(q->bytes_flushed_last + charged_bytes);
    q->bytes_flushed_total = (uint16_t)(q->bytes_flushed_total + charged_bytes);
}

uint16_t vfx_job_cost(const VfxJob *job) {
    if (!job) return 0u;
    if (job->kind == VFX_JOB_BG_TILE || job->kind == VFX_JOB_SPRITE_TILE) return 32u;
    if (job->kind == VFX_JOB_BG_CELL) return 2u;
    return 0u;
}
