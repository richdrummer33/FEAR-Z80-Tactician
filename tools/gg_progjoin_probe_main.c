#include <stdint.h>
#include <gbdk/platform.h>
#include "pj_assets.h"
#include "pj_vectors.h"

#define PJ_C 6u
#define PJ_BANK_BYTES 16384u
#define PJ_GUARD_ROWS 7u
#define PJ_COLS 20u
#define PJ_BUF_BYTES (32u * PJ_COLS * 2u)

volatile uint8_t g_pj_probe_done;
volatile uint16_t g_pj_probe_fail;
volatile uint16_t g_pj_probe_lookup_miss;
volatile uint16_t g_pj_probe_bounds_fail;
volatile uint16_t g_pj_probe_cases_done;
volatile uint16_t g_pj_probe_last_case;
volatile uint32_t g_pj_probe_last_hash;
volatile uint32_t g_pj_probe_expect_hash;

static uint8_t g_pj_buf[PJ_BUF_BYTES];

static uint16_t rd16p(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint8_t record_byte(uint16_t off) {
    if (off < PJ_BANK_BYTES) {
        SWITCH_ROM2(10u);
        return gg_pj_records0[off];
    }
    SWITCH_ROM2(11u);
    return gg_pj_records1[(uint16_t)(off - PJ_BANK_BYTES)];
}

static const uint8_t *body_base(uint8_t logical_bank) {
    switch (logical_bank) {
        case 0u: SWITCH_ROM2(12u); return gg_pj_body0;
        case 1u: SWITCH_ROM2(13u); return gg_pj_body1;
        case 2u: SWITCH_ROM2(14u); return gg_pj_body2;
        case 3u: SWITCH_ROM2(15u); return gg_pj_body3;
        case 4u: SWITCH_ROM2(16u); return gg_pj_body4;
        default: return (const uint8_t *)0;
    }
}

/* Return 1 and the direct body location when the sparse selector contains the
 * exact target semantic key. Missing entries deliberately mean old-path fallback
 * in the later renderer integration; the standalone probe treats one as failure. */
static uint8_t dispatch_body(int16_t step, uint8_t fam, uint8_t want,
                             int16_t iq, uint8_t *out_bank, uint16_t *out_off) {
    uint16_t step_index;
    uint8_t page, local_rank, rank, u, base, fi, key;
    uint16_t slot, desc_i, rec;
    int16_t acc;
    const uint8_t *p;

    if ((fam != 0u && fam != 2u) || want == 0u || want > PJ_C) return 0u;
    if (step < -2048 || step > 2047) return 0u;
    step_index = (uint16_t)((int16_t)(step + 2048));
    page = (uint8_t)(step_index >> 8);

    SWITCH_ROM2(8u);
    local_rank = gg_pj_step_local[step_index];
    if (local_rank == 0xFFu) return 0u;
    slot = (uint16_t)(rd16p(gg_pj_step_page_base + ((uint16_t)page << 1)) + local_rank);

    /* Exact target dispatcher: u=(iq+32)&127 and rank is the number of the
     * sorted C+1 thresholds already crossed. */
    acc = (int16_t)(iq + 32);
    u = (uint8_t)acc & 127u;
    rank = 0u;
    p = gg_pj_thresholds + ((uint16_t)slot << 3);
    while (rank < (PJ_C + 1u) && u >= p[rank]) ++rank;
    base = (uint8_t)(((int16_t)acc >> 7) & 7);

    fi = (fam == 0u) ? 0u : 1u;
    desc_i = (uint16_t)((slot * 12u + (uint16_t)fi * PJ_C + (uint16_t)(want - 1u)) << 1);
    SWITCH_ROM2(9u);
    rec = rd16p(gg_pj_descriptor + desc_i);
    if (rec == 0xFFFFu) return 0u;

    key = (uint8_t)((base << 3) | rank);
    for (;;) {
        uint8_t got = record_byte(rec);
        if (got == 0xFFu) return 0u;
        if (got == key) {
            *out_bank = record_byte((uint16_t)(rec + 1u));
            *out_off = (uint16_t)record_byte((uint16_t)(rec + 2u)) |
                       ((uint16_t)record_byte((uint16_t)(rec + 3u)) << 8);
            return 1u;
        }
        rec = (uint16_t)(rec + 4u);
    }
}

static uint8_t play_body(uint8_t bank, uint16_t off, int16_t *cursor) {
    const uint8_t *basep = body_base(bank);
    const uint8_t *p;
    uint8_t n, i;
    if (!basep || off >= PJ_BANK_BYTES) return 0u;
    p = basep + off;
    n = *p++;
    for (i = 0u; i != n; ++i) {
        uint16_t word;
        int16_t delta;
        int16_t c = *cursor;
        if (c < 0 || c >= (int16_t)(PJ_BUF_BYTES - 1u)) {
            ++g_pj_probe_bounds_fail;
            return 0u;
        }
        word = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        delta = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        p += 4;
        g_pj_buf[(uint16_t)c] = (uint8_t)word;
        g_pj_buf[(uint16_t)c + 1u] = (uint8_t)(word >> 8);
        /* Z80 target leaves HL on the high byte, then ADD HL,delta. */
        *cursor = (int16_t)(c + 1 + delta);
    }
    return 1u;
}

static uint32_t fnv1a32(void) {
    uint16_t i;
    uint32_t h = 2166136261UL;
    for (i = 0u; i != PJ_BUF_BYTES; ++i) {
        h ^= g_pj_buf[i];
        h *= 16777619UL;
    }
    return h;
}

static uint8_t run_case(const GGPJProbeCase *tc) {
    uint16_t i;
    int16_t iq = tc->iq0;
    int16_t cursor = (int16_t)(PJ_GUARD_ROWS * PJ_COLS * 2u + tc->first_dest);
    uint8_t left = tc->ncol;

    for (i = 0u; i != PJ_BUF_BYTES; ++i) g_pj_buf[i] = 0x5Au;

    while (left != 0u) {
        uint8_t want = (left > PJ_C) ? PJ_C : left;
        uint8_t bank;
        uint16_t off;
        if (!dispatch_body(tc->step, tc->fam, want, iq, &bank, &off)) {
            ++g_pj_probe_lookup_miss;
            return 0u;
        }
        if (!play_body(bank, off, &cursor)) return 0u;
        iq = (int16_t)(iq + (int16_t)((int16_t)want * tc->step));
        left = (uint8_t)(left - want);
    }

    g_pj_probe_last_hash = fnv1a32();
    g_pj_probe_expect_hash = tc->expect_hash;
    return g_pj_probe_last_hash == tc->expect_hash;
}

void main(void) {
    uint16_t i;
    DISPLAY_OFF;
    g_pj_probe_done = 0u;
    g_pj_probe_fail = 0u;
    g_pj_probe_lookup_miss = 0u;
    g_pj_probe_bounds_fail = 0u;
    g_pj_probe_cases_done = 0u;
    g_pj_probe_last_case = 0u;
    g_pj_probe_last_hash = 0u;
    g_pj_probe_expect_hash = 0u;

    for (i = 0u; i != GG_PJ_PROBE_CASE_COUNT; ++i) {
        g_pj_probe_last_case = i;
        if (!run_case(&gg_pj_probe_cases[i])) ++g_pj_probe_fail;
        g_pj_probe_cases_done = (uint16_t)(i + 1u);
    }
    g_pj_probe_done = 1u;
    for (;;) { /* host debugger observes the result symbols */ }
}
