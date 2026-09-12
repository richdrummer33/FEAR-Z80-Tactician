/*
 * EDGE_RING: generate the dense phase rings analytically and deduplicate.
 *
 * edge_cycle_probe confirmed the state model: given
 *     (step mod 1024, step >> 10, profile)   and   phase = (iq+32) mod 1024
 * the emitted (tile code, rows spanned, row advance) is deterministic to
 * 70 conflicts in 873,084 columns (0.008%).
 *
 * Observed phases are not enough to bake from: only 3.39% of a class's 1024
 * phases are ever seen in the corpus, but a ring must be COMPLETE or a pose
 * that enters at an unbaked phase is wrong.  So the rings are generated
 * analytically for every phase, then deduplicated exactly.
 *
 * This is the honest ROM number for the architecture.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define PHASES 1024

/* One entry of a ring, position-independent. */
static uint16_t entry_for(int phase, int smod, int whole, int riser) {
    /* Put the accumulator high enough that the 255 inverse-depth clamp cannot
     * fire, so the ring is the unclamped truth; screen clamping is applied
     * separately by the caller at run time. */
    int a = 4 * 1024 + phase;
    int step = whole * 1024 + smod;
    int32_t raw_l = (int32_t)a >> 6, raw_r = (int32_t)(a + step) >> 6;
    uint8_t invl = (uint8_t)clamp_u8i((int16_t)raw_l, 255u);
    uint8_t invr = (uint8_t)clamp_u8i((int16_t)raw_r, 255u);
    uint8_t hl = (uint8_t)(invl >> 1), hr = (uint8_t)(invr >> 1);
    int16_t tl, tr;
    int8_t slope, r0, r1;
    uint16_t tile, code;
    if (riser) { tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                 tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2)); }
    else       { tl = (int16_t)(TSPF_HORIZON - hl);
                 tr = (int16_t)(TSPF_HORIZON - hr); }
    slope = clamp_s8((int16_t)(tr - tl), -7, 7);
    r0 = row_floor(tl < tr ? tl : tr);
    r1 = row_floor(tl > tr ? tl : tr);
    tile = edge_entry(0u, (int16_t)(tl - ((int16_t)r0 << 3)), slope, 0u);
    code = (uint16_t)((tile - TSP_TILE_EDGE_BASE) & 0x7ff);
    return (uint16_t)(code
         | ((uint16_t)((r1 - r0) & 3) << 11)
         | ((uint16_t)((((int)(hl >> 3) - (int)(hr >> 3)) + 4) & 7) << 13));
}

typedef struct { uint64_t h; unsigned n; } Uniq;
#define UN (1u << 20)
static Uniq g_u[UN];
static unsigned g_nuniq;

static int add_uniq(uint64_t h) {
    unsigned i = (unsigned)((h * 1181783497276652981ull) >> 44) & (UN - 1u);
    for (;;) {
        if (!g_u[i].h) { g_u[i].h = h ? h : 1ull; g_u[i].n = 1; ++g_nuniq; return 1; }
        if (g_u[i].h == (h ? h : 1ull)) { ++g_u[i].n; return 0; }
        i = (i + 1u) & (UN - 1u);
    }
}

static uint64_t hash_ring(const uint16_t *r) {
    uint64_t h = 1469598103934665603ull; int i;
    for (i = 0; i < PHASES; ++i) { h ^= r[i]; h *= 1099511628211ull; }
    return h;
}

int main(void) {
    static uint16_t ring[PHASES];
    int smod, whole, riser;
    unsigned long total = 0;
    unsigned long uniq_all, uniq_full, uniq_rot;

    /* 1. every class, both top-edge families */
    for (riser = 0; riser < 2; ++riser)
        for (whole = -2; whole <= 2; ++whole)
            for (smod = 0; smod < PHASES; ++smod) {
                int p;
                for (p = 0; p < PHASES; ++p) ring[p] = entry_for(p, smod, whole, riser);
                add_uniq(hash_ring(ring));
                ++total;
            }
    uniq_all = g_nuniq;

    /* 2. the same, restricted to the non-RISER family (3 of 4 profiles) */
    memset(g_u, 0, sizeof g_u); g_nuniq = 0;
    for (whole = -2; whole <= 2; ++whole)
        for (smod = 0; smod < PHASES; ++smod) {
            int p;
            for (p = 0; p < PHASES; ++p) ring[p] = entry_for(p, smod, whole, 0);
            add_uniq(hash_ring(ring));
        }
    uniq_full = g_nuniq;

    /* 3. rotation-equivalence: a ring is walked from an arbitrary entry phase,
     *    so two rings that differ only by a rotation are NOT interchangeable
     *    unless the entry offset is also adjusted.  Counted separately so the
     *    saving is not claimed for free. */
    memset(g_u, 0, sizeof g_u); g_nuniq = 0;
    for (whole = -2; whole <= 2; ++whole)
        for (smod = 0; smod < PHASES; ++smod) {
            int p; uint64_t best = ~0ull;
            for (p = 0; p < PHASES; ++p) ring[p] = entry_for(p, smod, whole, 0);
            /* canonical form: minimum hash over all rotations is too slow to do
             * exactly here, so use the multiset hash as a necessary condition */
            {
                uint64_t m = 0; int i;
                for (i = 0; i < PHASES; ++i) m += (uint64_t)ring[i] * 1099511628211ull;
                best = m;
            }
            add_uniq(best);
        }
    uniq_rot = g_nuniq;

    printf("=== EDGE_RING: dense rings, generated analytically and deduplicated ===\n\n");
    printf("classes enumerated (smod 0..1023 x whole -2..2 x 2 edge families) %lu\n\n",
           total);
    printf("  DISTINCT rings, both families          %lu\n", uniq_all);
    printf("  DISTINCT rings, non-RISER family only  %lu\n", uniq_full);
    printf("  upper bound on rotation classes        %lu  (multiset hash, a\n"
           "                                              NECESSARY condition\n"
           "                                              only - not a usable\n"
           "                                              dedup by itself)\n\n",
           uniq_rot);

    printf("ROM for the dense rings (1024 entries each)\n");
    printf("  %-40s %11s %11s %11s\n", "", "1 B/entry", "2 B/entry", "3 B/entry");
    printf("  %-40s %10.2fM %10.2fM %10.2fM\n", "all distinct rings",
           uniq_all * 1024.0 / 1048576.0, uniq_all * 2048.0 / 1048576.0,
           uniq_all * 3072.0 / 1048576.0);
    printf("  %-40s %10.2fM %10.2fM %10.2fM\n", "non-RISER family only",
           uniq_full * 1024.0 / 1048576.0, uniq_full * 2048.0 / 1048576.0,
           uniq_full * 3072.0 / 1048576.0);
    printf("  %-40s %10.2fM %10.2fM %10.2fM\n", "if whole-tile part collapsed (smod only)",
           1024 * 1024.0 / 1048576.0, 1024 * 2048.0 / 1048576.0,
           1024 * 3072.0 / 1048576.0);

    /* 4. split the entry: does the TILE CODE alone dedup across the whole-tile
     *    part?  When |step| >= 1024 the slope saturates at +/-7, so the tile
     *    may be independent of `whole` even though the row advance is not. */
    {
        unsigned long uniq_tile, uniq_adv;
        memset(g_u, 0, sizeof g_u); g_nuniq = 0;
        for (whole = -2; whole <= 2; ++whole)
            for (smod = 0; smod < PHASES; ++smod) {
                int p;
                for (p = 0; p < PHASES; ++p)
                    ring[p] = (uint16_t)(entry_for(p, smod, whole, 0) & 0x7ff);
                add_uniq(hash_ring(ring));
            }
        uniq_tile = g_nuniq;

        memset(g_u, 0, sizeof g_u); g_nuniq = 0;
        for (whole = -2; whole <= 2; ++whole)
            for (smod = 0; smod < PHASES; ++smod) {
                int p;
                for (p = 0; p < PHASES; ++p)
                    ring[p] = (uint16_t)(entry_for(p, smod, whole, 0) >> 11);
                add_uniq(hash_ring(ring));
            }
        uniq_adv = g_nuniq;

        printf("\nSPLIT entry: tile code and row advance stored separately\n");
        printf("  distinct TILE-CODE rings (of 5120)     %lu\n", uniq_tile);
        printf("  distinct ROW-ADVANCE rings (of 5120)   %lu\n", uniq_adv);
        printf("  tile codes fit in 1 byte? offsets %u x slopes %u = %u values\n",
               TSP_EDGE_OFF_COUNT, TSP_EDGE_SLOPE_COUNT,
               TSP_EDGE_OFF_COUNT * TSP_EDGE_SLOPE_COUNT);
        printf("  ROM: tile rings at 1 B/entry           %.2f MB\n",
               uniq_tile * 1024.0 / 1048576.0);
        printf("  ROM: advance rings at 1 B/entry        %.2f MB\n",
               uniq_adv * 1024.0 / 1048576.0);
        printf("  ROM: both                              %.2f MB\n",
               (uniq_tile + uniq_adv) * 1024.0 / 1048576.0);
    }
    printf("\n  4 MB cartridge = %.2f MB usable for this alone\n", 4.0);
    return 0;
}
