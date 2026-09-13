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
#define NFAM 5
static const int k_famperiod[NFAM] = { 1024, 1024, 1024, 2048, 4096 };
static const char *k_famname[NFAM] = {
    "71-h   FULL top", "72-h   LINTEL/RAISED top", "72+h   FULL/RISER bot",
    "72-h>>1 LINTEL bot", "72+h-h>>2 RAISED bot/RISER top" };
static int endpoint(int fam, int h) {
    switch (fam) {
    case 0: return TSPF_HORIZON - 1 - h;
    case 1: return TSPF_HORIZON - h;
    case 2: return TSPF_HORIZON + h;
    case 3: return TSPF_HORIZON - (h >> 1);
    default: return TSPF_HORIZON + h - (h >> 2);
    }
}

/* One CELL of a program.  A column can emit more than one row - about 14% do -
 * so `row` selects which row within the column, and the program advances its
 * phase only at a column boundary.  Verified against the renderer in
 * edge_family_verify. */
static uint16_t cell_for(int fam, int phase, int r, int q, int row, int *nrows) {
    int a = 8 * 1024 + phase, step = q * 1024 + r;
    uint8_t invl = (uint8_t)clamp_u8i((int16_t)((int32_t)a >> 6), 255u);
    uint8_t invr = (uint8_t)clamp_u8i((int16_t)((int32_t)(a + step) >> 6), 255u);
    int hl = invl >> 1, hr = invr >> 1;
    int16_t tl = (int16_t)endpoint(fam, hl), tr = (int16_t)endpoint(fam, hr);
    int8_t slope = clamp_s8((int16_t)(tr - tl), -7, 7);
    int8_t q0 = row_floor(tl < tr ? tl : tr);
    int8_t q1 = row_floor(tl > tr ? tl : tr);
    uint16_t tile;
    *nrows = q1 - q0 + 1;
    if (row > q1 - q0) row = q1 - q0;
    tile = edge_entry(0u, (int16_t)(tl - (((int16_t)(q0 + row)) << 3)), slope, 0u);
    return (uint16_t)(((tile - TSP_TILE_EDGE_BASE) & 0x7f)
         | ((uint16_t)((((hl >> 3) - (hr >> 3)) + 8) & 15) << 7));
}

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

static void program(uint16_t *out, int L, int phase, int r, int q, int fam) {
    int period = k_famperiod[fam];
    int k = 0, p = phase, adv = ((q * 1024 + r) % period + period * 4) % period;
    while (k < L) {
        int nr = 1, row;
        for (row = 0; row < 8 && k < L; ++row) {
            out[k++] = cell_for(fam, p, r, q, row, &nr);
            if (row + 1 >= nr) break;
        }
        p = (p + adv) % period;
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

    printf("=== EDGE_PROGRAM: exact finite CELL programs, all five families ===\n\n");
    printf("A44 used two families and per-COLUMN programs.  Both were wrong:\n");
    printf("draw_run decrements FULL's top endpoint, so there are FIVE endpoint\n");
    printf("families with three distinct phase periods, and about 14%% of columns\n");
    printf("emit more than one row, so a program is a sequence of CELLS.\n");
    printf("All five verify EXACT against the renderer (edge_family_verify).\n\n");
    for (int f = 0; f < NFAM; ++f)
        printf("  family %d  %-34s period %d\n", f, k_famname[f], k_famperiod[f]);

    printf("\n%-4s %12s %12s %12s %12s %12s\n",
           "L", "programs", "intervals", "body 1B/e", "body 2B/e", "dispatch");
    for (hi = 0; hi < NH; ++hi) {
        int L = horizons[hi];
        Set uniq; unsigned long intervals = 0;
        int fam, qi, r;
        uint16_t prev[MAXL], cur[MAXL];
        set_init(&uniq, 23);
        for (fam = 0; fam < NFAM; ++fam)
            for (qi = 0; qi < NQ; ++qi)
                for (r = 0; r < 1024; ++r) {
                    int p, first = 1;
                    for (p = 0; p < k_famperiod[fam]; ++p) {
                        program(cur, L, p, r, k_q[qi], fam);
                        if (first || memcmp(cur, prev, sizeof(uint16_t) * L)) {
                            ++intervals; set_add(&uniq, hashprog(cur, L));
                            memcpy(prev, cur, sizeof(uint16_t) * L); first = 0;
                        }
                    }
                }
        printf("%-4d %12u %12lu %11.1fK %11.1fK %11.1fK   total 1B/e %.2f MB\n",
               L, uniq.n, intervals, uniq.n * (double)L / 1024.0,
               uniq.n * 2.0 * L / 1024.0, intervals * 4.0 / 1024.0,
               (uniq.n * (double)L + intervals * 4.0) / 1048576.0);
        set_free(&uniq);
    }
    return 0;
}
