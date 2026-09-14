#include <stdint.h>
#include <gbdk/platform.h>
#include "pj_vectors.h"
#include "tilesector_polar_progjoin_runtime.h"

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

static uint32_t fnv1a32(void) {
    uint16_t i;
    uint32_t h = 2166136261UL;
    for (i = 0u; i != PJ_BUF_BYTES; ++i) {
        h ^= g_pj_buf[i];
        h *= 16777619UL;
    }
    return h;
}

/* Snapshot every vector field before the first Frame-2 switch. This is a
 * deliberate cartridge-lifetime rule, not just a probe convenience: no live
 * pointer into ROM data may survive arbitrary mapper changes. */
static uint8_t run_case(const GGPJProbeCase *tc) {
    uint16_t i;
    const int16_t step = tc->step;
    const uint8_t fam = tc->fam;
    const uint8_t ncol = tc->ncol;
    const int16_t iq0 = tc->iq0;
    int16_t cursor = (int16_t)(PJ_GUARD_ROWS * PJ_COLS * 2u + tc->first_dest);
    const uint32_t expect_hash = tc->expect_hash;
    TSPProgjoinRunPlan plan;

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
    return g_pj_probe_last_hash == expect_hash;
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
    for (;;) { }
}
