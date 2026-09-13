/*
 * EDGE_DISPATCH_VERIFY: is the phase->program map a (base, rank) decomposition?
 *
 * A45 measured the dispatch at 3.00 MB of ROM and 1,593 T per lookup, making it
 * both the dominant ROM cost and the dominant remaining CPU cost.  The
 * prediction was that the interval breakpoints are not arbitrary.
 *
 * Derivation.  Let a be the accumulator and step the per-column advance.  Then
 *     h_k = (a + k*step) >> 7
 * and writing a = 128*H + u with u in [0,128):
 *     h_k = H + ((u + k*step) >> 7)
 * so the sequence of heights relative to H depends on u only through which of
 * the thresholds
 *     t_k = (-k*step) mod 128,      k = 1 .. C-1
 * the value u has passed.  Those thresholds sorted give a RANK in 0..C-1, and
 * the tile additionally needs H modulo the family's base count (because the
 * endpoint formula repeats every M pixels).  So the prediction is
 *
 *     program = T[family][step][H mod M][rank(u)]
 *
 * with M = period/128, i.e. 8, 8, 8, 16, 32 for the five families.
 *
 * This proves or refutes that over the whole domain, not a sample.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define NFAM 5
static const int k_period[NFAM] = { 1024, 1024, 1024, 2048, 4096 };
static const char *k_famname[NFAM] = {
    "71-h      FULL top", "72-h      LINTEL/RAISED top",
    "72+h      FULL/RISER bot", "72-(h>>1) LINTEL bot",
    "72+h-h>>2 RAISED bot/RISER top" };

static int endpoint(int fam, int h) {
    switch (fam) {
    case 0: return TSPF_HORIZON - 1 - h;
    case 1: return TSPF_HORIZON - h;
    case 2: return TSPF_HORIZON + h;
    case 3: return TSPF_HORIZON - (h >> 1);
    default: return TSPF_HORIZON + h - (h >> 2);
    }
}

/* One column's contribution: up to 3 cells, plus the row advance. */
static int g_noclamp = 0;
static int g_clamp_hit = 0;

static int column_cells(int fam, int a, int step, uint16_t *out) {
    int rl = (int)((int32_t)a >> 6), rr2 = (int)((int32_t)(a + step) >> 6);
    uint8_t invl, invr;
    int hl, hr, row, n;
    if (rl < 0 || rl > 255 || rr2 < 0 || rr2 > 255) g_clamp_hit = 1;
    if (g_noclamp) { hl = rl >> 1; hr = rr2 >> 1; }
    else {
        invl = (uint8_t)clamp_u8i((int16_t)rl, 255u);
        invr = (uint8_t)clamp_u8i((int16_t)rr2, 255u);
        hl = invl >> 1; hr = invr >> 1;
    }
    {
    int16_t tl = (int16_t)endpoint(fam, hl), tr = (int16_t)endpoint(fam, hr);
    int8_t slope = clamp_s8((int16_t)(tr - tl), -7, 7);
    int8_t q0 = row_floor(tl < tr ? tl : tr);
    int8_t q1 = row_floor(tl > tr ? tl : tr);
    n = q1 - q0 + 1;
    if (n > 4) n = 4;
    for (row = 0; row < n; ++row) {
        uint16_t t = edge_entry(0u, (int16_t)(tl - (((int16_t)(q0 + row)) << 3)),
                                slope, 0u);
        out[row] = (uint16_t)(((t - TSP_TILE_EDGE_BASE) & 0x7f)
                 | ((uint16_t)((((hl >> 3) - (hr >> 3)) + 8) & 15) << 7));
    }
    return n;
    }
}

/* the C-column program, as a hash, from a representative accumulator */
static uint64_t prog_hash(int fam, int phase, int step, int C) {
    uint64_t h = 1469598103934665603ull;
    int a = 8 * 1024 + phase, c, i, n;
    uint16_t cells[4];
    for (c = 0; c < C; ++c) {
        n = column_cells(fam, a, step, cells);
        h ^= (uint64_t)n; h *= 1099511628211ull;
        for (i = 0; i < n; ++i) { h ^= cells[i]; h *= 1099511628211ull; }
        a += step;
    }
    return h ? h : 1ull;
}

/* distinct C-column programs, so the body cost can be reported beside the
 * dispatch cost rather than assumed small */
#define PN (1u << 22)
static uint64_t *g_pt; static unsigned long g_pn;
static void prog_add(uint64_t h) {
    unsigned i = (unsigned)((h * 1181783497276652981ull) >> 42) & (PN - 1u);
    for (;;) {
        if (!g_pt[i]) { g_pt[i] = h; ++g_pn; return; }
        if (g_pt[i] == h) return;
        i = (i + 1u) & (PN - 1u);
    }
}

int main(int argc, char **argv) {
    int C = (argc > 1) ? atoi(argv[1]) : 4;
    g_noclamp = (argc > 2) ? atoi(argv[2]) : 0;
    int fam, step, P, u, k;
    int thr[8], order[8];
    printf("=== EDGE_DISPATCH_VERIFY: program = T[fam][step][H mod M][rank] ===\n");
    printf("C = %d columns per program, step over the full [-2048,2047] range\n\n", C);
    printf("%-34s %4s %14s %12s %12s\n", "family", "M", "observations",
           "conflicts", "table entries");

    unsigned long grand_entries = 0, grand_conf = 0;
    g_pt = calloc(PN, sizeof(uint64_t));
    if (!g_pt) { fprintf(stderr, "oom\n"); return 1; }
    for (fam = 0; fam < NFAM; ++fam) {
        int M = k_period[fam] / 128;
        unsigned long obs = 0, conf = 0, entries = 0, clamped = 0;
        for (step = -2048; step < 2048; ++step) {
            /* Thresholds, sorted.  A C-column program reads C+1 heights -
             * column c uses h_c and h_{c+1} - so it needs C thresholds, not
             * C-1.  The first draft used C-1 and failed on 12% of samples. */
            int nt = C;
            for (k = 1; k <= nt; ++k) {
                int t = (int)(((long)(-k) * step) % 128);
                if (t < 0) t += 128;
                thr[k - 1] = t;
            }
            for (k = 0; k < nt; ++k) order[k] = thr[k];
            for (k = 1; k < nt; ++k) {           /* insertion sort */
                int v = order[k], j = k - 1;
                while (j >= 0 && order[j] > v) { order[j + 1] = order[j]; --j; }
                order[j + 1] = v;
            }
            for (P = 0; P < M; ++P) {
                uint64_t seen[9]; int have[9];
                memset(have, 0, sizeof have);
                for (k = 0; k <= nt; ++k) seen[k] = 0;
                for (u = 0; u < 128; ++u) {
                    int rank = 0;
                    uint64_t h;
                    for (k = 0; k < nt; ++k) if (u >= order[k]) ++rank;
                    g_clamp_hit = 0;
                    h = prog_hash(fam, 128 * P + u, step, C);
                    if (g_clamp_hit) { ++clamped; continue; }
                    ++obs; prog_add(h);
                    if (!have[rank]) { have[rank] = 1; seen[rank] = h; }
                    else if (seen[rank] != h) ++conf;
                }
                for (k = 0; k <= nt; ++k) if (have[k]) ++entries;
            }
        }
        printf("%-34s %4d %14lu %12lu %12lu %s  (clamped, skipped: %lu)\n",
               k_famname[fam], M, obs, conf, entries,
               conf ? "*** FAILS ***" : "EXACT", clamped);
        grand_entries += entries; grand_conf += conf;
    }
    printf("\ntotal table entries %lu, conflicts %lu\n", grand_entries, grand_conf);
    printf("dispatch ROM at 2 B/entry            %.2f MB\n",
           grand_entries * 2.0 / 1048576.0);
    printf("threshold lists, %d bytes per (fam,step)  %.1f KB\n", C,
           5.0 * 4096 * C / 1024.0);
    printf("distinct %d-column programs          %lu\n", C, g_pn);
    printf("program bodies at ~1.2 cells/col, 1 B/cell  %.1f KB\n",
           g_pn * C * 1.2 / 1024.0);
    printf("TOTAL dispatch + thresholds + bodies %.2f MB\n",
           (grand_entries * 2.0 + 5.0 * 4096 * C + g_pn * C * 1.2) / 1048576.0);
    return 0;
}
