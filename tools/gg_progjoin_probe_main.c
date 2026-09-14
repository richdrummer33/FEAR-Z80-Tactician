#include <stdint.h>
#include <gbdk/platform.h>
#include "pj_vectors.h"
#include "tilesector_polar_progjoin_runtime.h"

#define PJ_GUARD_ROWS 7u
#define PJ_COLS 20u
#define PJ_ROWS 18u
#define PJ_ROW_BYTES (PJ_COLS * 2u)
#define PJ_BUF_BYTES (32u * PJ_ROW_BYTES)
#define PJ_VISIBLE_BYTES (PJ_ROWS * PJ_ROW_BYTES)

volatile uint8_t g_pj_probe_done;
volatile uint16_t g_pj_probe_fail;
volatile uint16_t g_pj_probe_lookup_miss;
volatile uint16_t g_pj_probe_bounds_fail;
volatile uint16_t g_pj_probe_gate_fail;
volatile uint16_t g_pj_probe_cases_done;
volatile uint16_t g_pj_probe_last_case;
volatile uint32_t g_pj_probe_last_hash;
volatile uint32_t g_pj_probe_expect_hash;

static uint8_t g_pj_buf[PJ_BUF_BYTES];
static uint16_t g_pj_live[PJ_ROWS * PJ_COLS];
static uint8_t g_pj_cov[60];
static uint8_t g_pj_row_min[PJ_ROWS];
static uint8_t g_pj_row_max[PJ_ROWS];

static uint32_t fnv1a32(void) {
    uint16_t i;
    uint32_t h = 2166136261UL;
    for (i = 0u; i != PJ_BUF_BYTES; ++i) {
        h ^= g_pj_buf[i];
        h *= 16777619UL;
    }
    return h;
}

static void reset_live(void) {
    uint16_t i;
    uint8_t *p = (uint8_t *)g_pj_live;
    for (i = 0u; i != PJ_VISIBLE_BYTES; ++i) p[i] = 0x5Au;
    for (i = 0u; i != PJ_ROWS; ++i) {
        g_pj_row_min[i] = 0xFFu;
        g_pj_row_max[i] = 0u;
    }
}

static void split_first_dest(int16_t d, int8_t *row, uint8_t *col) {
    int8_t r = 0;
    while (d < 0) { d = (int16_t)(d + PJ_ROW_BYTES); --r; }
    while (d >= (int16_t)PJ_ROW_BYTES) { d = (int16_t)(d - PJ_ROW_BYTES); ++r; }
    *row = r;
    *col = (uint8_t)((uint16_t)d >> 1);
}

static uint8_t compare_live_zero_ownership(void) {
    const uint8_t *raw = g_pj_buf + PJ_GUARD_ROWS * PJ_ROW_BYTES;
    const uint8_t *live = (const uint8_t *)g_pj_live;
    uint16_t i;
    for (i = 0u; i != PJ_VISIBLE_BYTES; ++i)
        if (raw[i] != live[i]) return 0u;
    return 1u;
}

static uint8_t compare_live_pattern_ownership(void) {
    const uint8_t *raw = g_pj_buf + PJ_GUARD_ROWS * PJ_ROW_BYTES;
    const uint8_t *live = (const uint8_t *)g_pj_live;
    uint8_t r, c;
    for (r = 0u; r != PJ_ROWS; ++r) {
        for (c = 0u; c != PJ_COLS; ++c) {
            uint8_t ci = (uint8_t)(c + c + c + (r >> 3));
            uint8_t owned = (uint8_t)(g_pj_cov[ci] & (uint8_t)(1u << (r & 7u)));
            uint16_t bi = (uint16_t)(((uint16_t)r * PJ_COLS + c) << 1);
            uint8_t e0 = owned ? 0x5Au : raw[bi];
            uint8_t e1 = owned ? 0x5Au : raw[(uint16_t)(bi + 1u)];
            if (live[bi] != e0 || live[(uint16_t)(bi + 1u)] != e1) return 0u;
        }
    }
    return 1u;
}

/* Snapshot every vector field before the first Frame-2 switch. This is a
 * deliberate cartridge-lifetime rule: no live pointer into switchable ROM data
 * may survive arbitrary mapper changes. */
static uint8_t run_case(const GGPJProbeCase *tc) {
    uint16_t i;
    const int16_t step = tc->step;
    const uint8_t fam = tc->fam;
    const uint8_t ncol = tc->ncol;
    const int16_t iq0 = tc->iq0;
    const int16_t first_dest = tc->first_dest;
    const uint32_t expect_hash = tc->expect_hash;
    int16_t cursor = (int16_t)(PJ_GUARD_ROWS * PJ_ROW_BYTES + first_dest);
    int8_t first_row;
    uint8_t first_col;
    TSPProgjoinRunPlan plan;

    split_first_dest(first_dest, &first_row, &first_col);
    for (i = 0u; i != PJ_BUF_BYTES; ++i) g_pj_buf[i] = 0x5Au;

    /* Same atomic contract the playable renderer will use: resolve the whole
     * run-edge before the first destination byte is modified. */
    if (!tsp_progjoin_preflight_run(step, fam, ncol, iq0, &plan)) {
        ++g_pj_probe_lookup_miss;
        return 0u;
    }
    if (!tsp_progjoin_play_plan(&plan, g_pj_buf, PJ_BUF_BYTES, &cursor)) {
        ++g_pj_probe_bounds_fail;
        return 0u;
    }

    g_pj_probe_last_hash = fnv1a32();
    g_pj_probe_expect_hash = expect_hash;
    if (g_pj_probe_last_hash != expect_hash) return 0u;

    /* Live gate test A: empty ownership must reproduce exactly the clipped
     * visible slice of the already host-validated guarded playback. */
    reset_live();
    for (i = 0u; i != 60u; ++i) g_pj_cov[i] = 0u;
    if (!tsp_progjoin_play_plan_gated(&plan, g_pj_live, g_pj_cov,
                                      g_pj_row_min, g_pj_row_max,
                                      first_row, first_col) ||
        !compare_live_zero_ownership()) {
        ++g_pj_probe_gate_fail;
        return 0u;
    }

    /* Live gate test B: deterministic partial ownership. Expected output is the
     * same exact visible oracle with owned cells left at the 0x5A sentinel. */
    reset_live();
    for (i = 0u; i != 60u; ++i)
        g_pj_cov[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 37u));
    if (!tsp_progjoin_play_plan_gated(&plan, g_pj_live, g_pj_cov,
                                      g_pj_row_min, g_pj_row_max,
                                      first_row, first_col) ||
        !compare_live_pattern_ownership()) {
        ++g_pj_probe_gate_fail;
        return 0u;
    }
    return 1u;
}

void main(void) {
    uint16_t i;
    DISPLAY_OFF;
    g_pj_probe_done = 0u;
    g_pj_probe_fail = 0u;
    g_pj_probe_lookup_miss = 0u;
    g_pj_probe_bounds_fail = 0u;
    g_pj_probe_gate_fail = 0u;
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
    for (;;) { }
}
