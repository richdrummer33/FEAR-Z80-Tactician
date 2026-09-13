/*
 * EDGE_PROGRAM: finite-horizon exact edge programs.
 *
 * A43 costed INFINITE 1024-phase rings at 6.76-20 MB.  That over-represents
 * the problem three ways, all of which are fixed here:
 *
 *  1. RISER was given its own sequence family.  It does not need one: it feeds
 *     the same edge masks.  What it does need is a LONGER phase domain - its
 *     top formula 72+h-(h>>2) makes tl mod 8 depend on h mod 32, so its period
 *     is 4096 accumulator units, not 1024.  Measured, not assumed.
 *
 *  2. The whole-tile part of step was given five complete ring families.  With
 *     step = 1024q + r, the row advance separates exactly as q + local, and the
 *     slope saturates at -/+7 for every q outside {0,-1} - verified
 *     exhaustively.  So only TWO q-classes need real rings and the rest are one
 *     tiny 8-entry table.  No clamping of the real step is involved.
 *
 *  3. The rings were infinite.  The screen is 20 coarse columns wide, so two
 *     sequences that diverge after the visible horizon are the SAME program.
 *
 * This bakes exact finite programs for several horizons and deduplicates them
 * globally, including interval (breakpoint) compression over starting phase and
 * a split prefix/shared-tail representation.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define MAXL 20
/* The observed range of the whole-tile part of step.  A42 measured |step| <
 * 2048 on 100% of columns, so q = floor(step/1024) lies in [-2, 1]; the probe
 * asserts this against the corpus rather than trusting it.  All four are
 * enumerated: for |q| >= 1 the SLOPE saturates, but the phase advance still
 * depends on q (and for RISER, whose period is 4096, on q mod 4), so a
 * saturated q does not collapse into a single tiny table the way a first
 * reading suggests. */
#define NQ 4
static const int k_q[NQ] = { -2, -1, 0, 1 };

/* ---------- a generic open-addressing uint64 set ---------- */
typedef struct { uint64_t *k; unsigned cap, n; } Set;
static void set_init(Set *s, unsigned bits) {
    s->cap = 1u << bits; s->n = 0;
    s->k = calloc(s->cap, sizeof(uint64_t));
    if (!s->k) { fprintf(stderr, "oom\n"); exit(1); }
}
static int set_add(Set *s, uint64_t h) {
    unsigned i = (unsigned)((h * 1181783497276652981ull) >> 40) & (s->cap - 1u);
    if (!h) h = 1;
    for (;;) {
        if (!s->k[i]) { s->k[i] = h; ++s->n; return 1; }
        if (s->k[i] == h) return 0;
        i = (i + 1u) & (s->cap - 1u);
    }
}
static void set_free(Set *s) { free(s->k); s->k = 0; }

/* ---------- one program entry ---------- */
/* tile code (7 bits: 16 offsets x 8 slopes) + local row advance (4 bits).
 * Position-independent and q-independent: the runtime adds q to the advance
 * and the destination cursor holds the absolute row. */
static uint16_t entry_for(int phase, int r, int q, int riser) {
    /* The high part of the accumulator must be chosen so the 255 inverse-depth
     * clamp does NOT fire: a>>6 must stay under 256, i.e. a < 16384.  The first
     * draft used 16*1024+phase, which is exactly AT the ceiling, so every
     * height pinned to 127 and only 2.4% of tiles matched.  8*1024 keeps
     * a>>6 in 128..175 for the whole step range.  The output is invariant in
     * this choice: hl = 8A + (phase>>7) exactly, so hl&7 and every difference
     * are independent of A, and local_left shifts by whole tiles. */
    int a = 8 * 1024 + phase;
    int step = q * 1024 + r;
    uint8_t invl = (uint8_t)clamp_u8i((int16_t)((int32_t)a >> 6), 255u);
    uint8_t invr = (uint8_t)clamp_u8i((int16_t)((int32_t)(a + step) >> 6), 255u);
    int hl = invl >> 1, hr = invr >> 1;
    int16_t tl, tr;
    int8_t slope, r0;
    uint16_t tile;
    if (riser) { tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                 tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2)); }
    else       { tl = (int16_t)(TSPF_HORIZON - hl);
                 tr = (int16_t)(TSPF_HORIZON - hr); }
    slope = clamp_s8((int16_t)(tr - tl), -7, 7);
    r0 = row_floor(tl < tr ? tl : tr);
    tile = edge_entry(0u, (int16_t)(tl - ((int16_t)r0 << 3)), slope, 0u);
    return (uint16_t)(((tile - TSP_TILE_EDGE_BASE) & 0x7f)
         | ((uint16_t)(((hl >> 3) - (hr >> 3) + 8) & 15) << 7));
}

static void program(uint16_t *out, int L, int phase, int r, int q, int riser,
                    int period) {
    int k, p = phase;
    for (k = 0; k < L; ++k) {
        out[k] = entry_for(p, r, q, riser);
        p = (p + ((q * 1024 + r) % period) + period * 4) % period;
    }
}

static uint64_t hashprog(const uint16_t *p, int n) {
    uint64_t h = 1469598103934665603ull; int i;
    for (i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

int main(void) {
    static const int horizons[] = { 4, 8, 12, 16, 20 };
    const int NH = (int)(sizeof horizons / sizeof horizons[0]);
    int hi;

    printf("=== EDGE_PROGRAM: exact finite programs for a 20-column screen ===\n\n");
    printf("phase domains: non-RISER 1024, RISER 4096 (measured, its top formula\n");
    printf("               72+h-(h>>2) makes tl mod 8 depend on h mod 32)\n");
    printf("q-classes needing real rings: 2 (q=0 and q=-1); every other q\n");
    printf("               saturates the slope at -/+7, verified exhaustively\n\n");

    printf("%-4s %12s %12s %12s %12s %12s\n",
           "L", "programs", "intervals", "body 1B/e", "body 2B/e", "dispatch");
    for (hi = 0; hi < NH; ++hi) {
        int L = horizons[hi];
        Set uniq;
        unsigned long intervals = 0;
        int riser, qi, r;
        uint16_t prev[MAXL], cur[MAXL];
        set_init(&uniq, 22);

        for (riser = 0; riser < 2; ++riser) {
            int period = riser ? 4096 : 1024;
            for (qi = 0; qi < NQ; ++qi)
                for (r = 0; r < 1024; ++r) {
                    int p, first = 1;
                    for (p = 0; p < period; ++p) {
                        program(cur, L, p, r, k_q[qi], riser, period);
                        if (first || memcmp(cur, prev, sizeof(uint16_t) * L)) {
                            ++intervals;
                            set_add(&uniq, hashprog(cur, L));
                            memcpy(prev, cur, sizeof(uint16_t) * L);
                            first = 0;
                        }
                    }
                }
        }
        printf("%-4d %12u %12lu %11.1fK %11.1fK %11.1fK\n", L, uniq.n, intervals,
               uniq.n * (double)L / 1024.0, uniq.n * 2.0 * L / 1024.0,
               intervals * 4.0 / 1024.0);
        set_free(&uniq);
        /* non-RISER alone: RISER's 4096-phase domain is 4x the scan, so it is
         * worth knowing what demoting it to a slow path would buy.  It is 6.0%
         * of emitted cells and the demotion is exact. */
        {
            Set u2; unsigned long iv2 = 0;
            set_init(&u2, 22);
            for (qi = 0; qi < NQ; ++qi)
                for (r = 0; r < 1024; ++r) {
                    int p, first = 1;
                    for (p = 0; p < 1024; ++p) {
                        program(cur, L, p, r, k_q[qi], 0, 1024);
                        if (first || memcmp(cur, prev, sizeof(uint16_t) * L)) {
                            ++iv2; set_add(&u2, hashprog(cur, L));
                            memcpy(prev, cur, sizeof(uint16_t) * L); first = 0;
                        }
                    }
                }
            printf("     non-RISER only: %u programs, %lu intervals, "
                   "body %.1fK + dispatch %.1fK = %.2f MB\n",
                   u2.n, iv2, u2.n * (double)L / 1024.0, iv2 * 4.0 / 1024.0,
                   (u2.n * (double)L + iv2 * 4.0) / 1048576.0);
            set_free(&u2);
        }
    }

    /* ---- split representation: prefix + shared tail, at L = 20 ---- */
    {
        int L = 20, cut = 8;
        Set tails, progs;
        int riser, qi, r;
        uint16_t prev[MAXL], cur[MAXL];
        set_init(&tails, 22); set_init(&progs, 22);
        for (riser = 0; riser < 2; ++riser) {
            int period = riser ? 4096 : 1024;
            for (qi = 0; qi < NQ; ++qi)
                for (r = 0; r < 1024; ++r) {
                    int p, first = 1;
                    for (p = 0; p < period; ++p) {
                        program(cur, L, p, r, k_q[qi], riser, period);
                        if (first || memcmp(cur, prev, sizeof(uint16_t) * L)) {
                            set_add(&progs, hashprog(cur, L));
                            set_add(&tails, hashprog(cur + cut, L - cut));
                            memcpy(prev, cur, sizeof(uint16_t) * L);
                            first = 0;
                        }
                    }
                }
        }
        printf("\nSPLIT at column %d (prefix stored per program, tail shared)\n", cut);
        printf("  distinct 20-column programs      %u\n", progs.n);
        printf("  distinct %d-column tails          %u\n", L - cut, tails.n);
        printf("  ROM 1 B/entry: %.1fK prefixes + %.1fK tails = %.2f MB\n",
               progs.n * (cut + 2.0) / 1024.0, tails.n * (double)(L - cut) / 1024.0,
               (progs.n * (cut + 2.0) + tails.n * (double)(L - cut)) / 1048576.0);
        set_free(&tails); set_free(&progs);
    }
    return 0;
}
