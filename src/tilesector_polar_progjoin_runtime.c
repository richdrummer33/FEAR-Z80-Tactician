#include <stdint.h>
#include <gbdk/platform.h>
#include "tilesector_polar_progjoin_runtime.h"
#include "pj_assets.h"

#define PJ_BANK_BYTES 16384u

static uint16_t rd16p(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint8_t record_byte(uint16_t off)
{
    if (off < PJ_BANK_BYTES)
    {
        SWITCH_ROM2(10u);
        return gg_pj_records0[off];
    }
    SWITCH_ROM2(11u);
    return gg_pj_records1[(uint16_t)(off - PJ_BANK_BYTES)];
}

static const uint8_t *body_base(uint8_t logical_bank)
{
    switch (logical_bank)
    {
    case 0u:
        SWITCH_ROM2(12u);
        return gg_pj_body0;
    case 1u:
        SWITCH_ROM2(13u);
        return gg_pj_body1;
    case 2u:
        SWITCH_ROM2(14u);
        return gg_pj_body2;
    case 3u:
        SWITCH_ROM2(15u);
        return gg_pj_body3;
    case 4u:
        SWITCH_ROM2(16u);
        return gg_pj_body4;
    default:
        return (const uint8_t *)0;
    }
}

uint8_t tsp_progjoin_dispatch_body(int16_t step, uint8_t fam, uint8_t want,
                                   int16_t iq, TSPProgjoinBodyRef *out)
{
    uint16_t step_index;
    uint8_t page, local_rank, rank, u, base, fi, key;
    uint16_t slot, desc_i, rec;
    int16_t acc;
    const uint8_t *p;

    if (!out || (fam != 0u && fam != 2u) || want == 0u || want > TSP_PROGJOIN_CHUNK_COLS)
        return 0u;
    if (step < -2048 || step > 2047)
        return 0u;

    step_index = (uint16_t)((int16_t)(step + 2048));
    page = (uint8_t)(step_index >> 8);

    SWITCH_ROM2(8u);
    local_rank = gg_pj_step_local[step_index];
    if (local_rank == 0xFFu)
        return 0u;
    slot = (uint16_t)(rd16p(gg_pj_step_page_base + ((uint16_t)page << 1)) + local_rank);

    /* Exact accepted target selector. */
    acc = (int16_t)(iq + 32);
    u = (uint8_t)acc & 127u;
    rank = 0u;
    p = gg_pj_thresholds + ((uint16_t)slot << 3);
    while (rank < (TSP_PROGJOIN_CHUNK_COLS + 1u) && u >= p[rank])
        ++rank;
    base = (uint8_t)(((int16_t)acc >> 7) & 7);

    fi = (fam == 0u) ? 0u : 1u;
    desc_i = (uint16_t)((slot * 12u + (uint16_t)fi * TSP_PROGJOIN_CHUNK_COLS + (uint16_t)(want - 1u)) << 1);
    SWITCH_ROM2(9u);
    rec = rd16p(gg_pj_descriptor + desc_i);
    if (rec == 0xFFFFu)
        return 0u;

    key = (uint8_t)((base << 3) | rank);
    for (;;)
    {
        uint8_t got = record_byte(rec);
        if (got == 0xFFu)
            return 0u;
        if (got == key)
        {
            out->bank = record_byte((uint16_t)(rec + 1u));
            out->off = (uint16_t)record_byte((uint16_t)(rec + 2u)) |
                       ((uint16_t)record_byte((uint16_t)(rec + 3u)) << 8);
            return 1u;
        }
        rec = (uint16_t)(rec + 4u);
    }
}

uint8_t tsp_progjoin_preflight_run(int16_t step, uint8_t fam, uint8_t ncol,
                                   int16_t iq0, TSPProgjoinRunPlan *out)
{
    uint8_t left, i;
    int16_t iq;

    if (!out || ncol == 0u || ncol > TSP_PROGJOIN_MAX_RUN_COLS)
        return 0u;

    out->count = 0u;
    left = ncol;
    iq = iq0;
    i = 0u;

    while (left)
    {
        uint8_t want = (left > TSP_PROGJOIN_CHUNK_COLS) ? TSP_PROGJOIN_CHUNK_COLS : left;
        if (i >= TSP_PROGJOIN_MAX_CHUNKS)
            return 0u;
        if (!tsp_progjoin_dispatch_body(step, fam, want, iq, &out->bodies[i]))
        {
            out->count = 0u;
            return 0u;
        }
        out->wants[i] = want;
        iq = (int16_t)(iq + (int16_t)((int16_t)want * step));
        left = (uint8_t)(left - want);
        ++i;
    }

    out->count = i;
    return 1u;
}

uint8_t tsp_progjoin_play_body(TSPProgjoinBodyRef body,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor)
{
    const uint8_t *basep = body_base(body.bank);
    const uint8_t *p;
    uint8_t n, i;

    if (!basep || !dst || !cursor || body.off >= PJ_BANK_BYTES || dst_bytes < 2u)
        return 0u;

    p = basep + body.off;
    n = *p++;
    for (i = 0u; i != n; ++i)
    {
        uint16_t word;
        int16_t delta;
        int16_t c = *cursor;
        if (c < 0 || c >= (int16_t)(dst_bytes - 1u))
            return 0u;

        word = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        delta = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        p += 4;
        dst[(uint16_t)c] = (uint8_t)word;
        dst[(uint16_t)c + 1u] = (uint8_t)(word >> 8);

        /* Accepted Z80 target leaves HL on the high byte, then ADD HL,delta. */
        *cursor = (int16_t)(c + 1 + delta);
    }
    return 1u;
}

uint8_t tsp_progjoin_play_plan(const TSPProgjoinRunPlan *plan,
                               uint8_t *dst, uint16_t dst_bytes,
                               int16_t *cursor)
{
    uint8_t i;
    if (!plan || plan->count == 0u || plan->count > TSP_PROGJOIN_MAX_CHUNKS)
        return 0u;
    for (i = 0u; i != plan->count; ++i)
        if (!tsp_progjoin_play_body(plan->bodies[i], dst, dst_bytes, cursor))
            return 0u;
    return 1u;
}
