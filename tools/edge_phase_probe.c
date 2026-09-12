/*
 * EDGE_PHASE: is the per-column edge evolution a small deterministic machine?
 *
 * A41 killed a walker that carried an endpoint and re-derived the tile.  It did
 * NOT test the stronger claim: that the whole per-column derivation is a
 * quantized recurrence which can be compiled.
 *
 * From the shipped draw_run (renderer line 475) the entire column state is one
 * accumulator:
 *
 *     a      = iq + 32                       advances by a constant `step`
 *     invl   = clamp(a >> 6, 255)            this column's left  inverse depth
 *     invr   = clamp((a+step) >> 6, 255)     ... and its right
 *     hl,hr  = invl>>1, invr>>1
 *     tl,tr  = 72 - hl, 72 - hr              (FULL; other profiles differ)
 *
 * and draw_edge selects its tile from (slope, local_left) where
 * slope = clamp(tr-tl) and local_left = tl - (row<<3).
 *
 * So split the accumulator:  a = 64*inv + phase.  Then
 *     phase' = (phase + step) & 63
 *     inv'   = inv + ((phase + step) >> 6)
 * and the emitted tile depends on inv only through inv & 15 (because
 * local_left needs hl & 7 and hl = inv>>1).  That makes the candidate minimal
 * exact state
 *
 *     (step, phase, inv & 15, profile, bottom)
 *
 * with a deterministic successor.  This probe does not assume that: it
 * enumerates what the shipped code actually emits and tests several candidate
 * states for determinism, then counts states, transitions and ROM.
 *
 * usage: edge_phase_probe [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

/* ---- observation table: state -> output, kept in a flat hash ---- */
#define HASHN (1u << 21)
typedef struct Obs {
    uint64_t key;        /* state */
    uint64_t out;        /* observed output + successor */
    uint32_t hits;
    uint8_t used;
} Obs;
static Obs *g_tab;
static unsigned long g_states, g_conflicts, g_obs;

static int record(uint64_t key, uint64_t out) {
    uint64_t h = key * 1181783497276652981ull;
    unsigned i = (unsigned)(h >> 43) & (HASHN - 1u);
    for (;;) {
        if (!g_tab[i].used) {
            g_tab[i].used = 1; g_tab[i].key = key; g_tab[i].out = out;
            g_tab[i].hits = 1; ++g_states; ++g_obs; return 1;
        }
        if (g_tab[i].key == key) {
            ++g_obs; ++g_tab[i].hits;
            if (g_tab[i].out != out) { ++g_conflicts; return 0; }
            return 1;
        }
        i = (i + 1u) & (HASHN - 1u);
    }
}

/* five candidate state definitions, tested in parallel */
#define NCAND 6
static const char *k_cand[NCAND] = {
    "(step, phase, inv&15, profile, bottom)",
    "(step, phase, inv,    profile, bottom)",
    "(      phase, inv&15, profile, bottom)   -- step dropped",
    "(step, phase,         profile, bottom)   -- inv dropped",
    "(step,        inv&15, profile, bottom)   -- phase dropped",
    "(step, phase, inv&15, profile) UNCLAMPED output",
};
static Obs *g_tabs[NCAND];
static unsigned long g_st[NCAND], g_cf[NCAND], g_ob[NCAND];

static void rec_c(int ci, uint64_t key, uint64_t out) {
    g_tab = g_tabs[ci];
    unsigned long s0 = g_states, c0 = g_conflicts, o0 = g_obs;
    g_states = g_st[ci]; g_conflicts = g_cf[ci]; g_obs = g_ob[ci];
    record(key, out);
    g_st[ci] = g_states; g_cf[ci] = g_conflicts; g_ob[ci] = g_obs;
    g_states = s0; g_conflicts = c0; g_obs = o0;
}

static unsigned long s_cols, s_cells, s_clamp_inv, s_clamp_row;
static unsigned long s_stepmag[20];
static unsigned long s_seq_total;
#define SEQN (1u << 22)
static uint64_t *g_seqtab; static unsigned long g_seq_distinct, g_seq_len_total;
static void seq_record(uint64_t h, unsigned len) {
    unsigned i = (unsigned)((h * 1181783497276652981ull) >> 42) & (SEQN - 1u);
    for (;;) {
        if (!g_seqtab[i]) { g_seqtab[i] = h ? h : 1ull; ++g_seq_distinct;
                            g_seq_len_total += len; return; }
        if (g_seqtab[i] == (h ? h : 1ull)) return;
        i = (i + 1u) & (SEQN - 1u);
    }
}
static unsigned long s_steps_seen[4096]; static unsigned s_nsteps;
static int s_stepvals[4096];
static unsigned long s_cells_by_step[4096];

static int step_slot(int step) {
    unsigned i;
    for (i = 0; i < s_nsteps; ++i) if (s_stepvals[i] == step) return (int)i;
    if (s_nsteps < 4096) { s_stepvals[s_nsteps] = step; return (int)s_nsteps++; }
    return -1;
}

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 8u;
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, i, j, ci;
    unsigned long poses = 0, positions = 0, cells_walkable = 0;

    g_seqtab = calloc(SEQN, sizeof(uint64_t));
    if (!g_seqtab) { fprintf(stderr, "oom\n"); return 1; }
    for (ci = 0; ci < NCAND; ++ci) {
        g_tabs[ci] = calloc(HASHN, sizeof(Obs));
        if (!g_tabs[ci]) { fprintf(stderr, "oom\n"); return 1; }
    }

    for (gy = 0; gy < GRID_H; ++gy)
    for (gx = 0; gx < GRID_W; ++gx) {
        int16_t px0 = (int16_t)(gx * CELL_Q4 + 32);
        int16_t py0 = (int16_t)(gy * CELL_Q4 + 32);
        uint16_t gi0 = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
        if (!tsp_is_walkable_q4(px0, py0)) continue;
        if (k_tspf_recipe_grid[gi0] == 0xffu) continue;
        ++cells_walkable;
        for (oi = 0; oi < n_off; ++oi) {
            int16_t px = (int16_t)(px0 + off[oi][0]);
            int16_t py = (int16_t)(py0 + off[oi][1]);
            if (!tsp_is_walkable_q4(px, py)) continue;
            ++positions;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                uint8_t ks[64], nk = 0, count = 0;
                uint8_t recipe, base_id, cond_count, lx, ly;
                uint16_t gi, offs;
                const uint8_t *p, *b;
                memset(&s, 0, sizeof s);
                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                gi = gi0;
                recipe = k_tspf_recipe_grid[gi];
                lx = (uint8_t)((uint16_t)px & 63u);
                ly = (uint8_t)((uint16_t)py & 63u);
                offs = k_tspf_recipe_off[recipe];
                p = &k_tspf_recipe_stream[offs];
                base_id = *p++; cond_count = *p++;
                b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
                i = *b++;
                for (; i; --i) ks[nk++] = *b++;
                for (i = 0; i < cond_count; ++i) {
                    uint8_t key = *p++, sel = *p++;
                    if (selector_pass(sel, lx, ly)) ks[nk++] = key;
                }
                tsp_polar_renderer_reset();
                g_corner_bearing_valid = 0u;
                for (j = 0; j < nk; ++j) {
                    if (count >= TSPF_MAX_ACTIVE) break;
                    if (!project_key(ks[j], &s, &g_runs[count])) continue;
                    insert_run(count, &count);
                }
                if (!count) continue;
                ++poses;

                for (i = 0; i < count; ++i) {
                    const PolarRun *r = &g_runs[g_run_order[i]];
                    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), c;
                    uint8_t profile = k_tspf_profile[r->sid];
                    int16_t iq, step, n;
                    int slot;
                    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
                    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
                    if (c1 < c0) continue;
                    n = (int16_t)(c1 - c0 + 1);
                    if (r->depth_plane) { c0 = r->c0; c1 = r->c1; n = (int16_t)(c1 - c0 + 1);
                                          iq = r->iq; step = r->step; }
                    else { iq = (int16_t)((int16_t)r->inv0 << 6);
                           step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                                            * (int16_t)k_col_recip_q8[n]);
                           step = shr_signed(step, 2); }
                    slot = step_slot(step);

                    uint64_t seqh = 1469598103934665603ull; unsigned seqlen = 0;
                    for (c = c0; c <= c1; ++c) {
                        int16_t a = (int16_t)(iq + 32);
                        int32_t raw_l = (int32_t)a >> 6, raw_r = (int32_t)(a + step) >> 6;
                        uint8_t invl = (uint8_t)clamp_u8i((int16_t)raw_l, 255u);
                        uint8_t invr = (uint8_t)clamp_u8i((int16_t)raw_r, 255u);
                        uint8_t hl = (uint8_t)(invl >> 1), hr = (uint8_t)(invr >> 1);
                        int16_t tl = (int16_t)(TSPF_HORIZON - hl);
                        int16_t tr = (int16_t)(TSPF_HORIZON - hr);
                        int8_t slope = clamp_s8((int16_t)(tr - tl), -7, 7);
                        int8_t r0 = row_floor(tl < tr ? tl : tr);
                        int8_t r1 = row_floor(tl > tr ? tl : tr);
                        int8_t r0c = r0, r1c = r1;
                        unsigned phase = (unsigned)(a & 63);
                        uint64_t out, k;
                        int16_t an;
                        unsigned phn; int invn;

                        unsigned phn_pre; int invn_pre;
                        { int16_t ap = (int16_t)(a + step);
                          phn_pre = (unsigned)(ap & 63);
                          invn_pre = (int)((int32_t)ap >> 6);
                          if (invn_pre < 0) invn_pre = 0;
                          if (invn_pre > 255) invn_pre = 255; }
                        if (raw_l != (int32_t)invl || raw_r != (int32_t)invr) ++s_clamp_inv;
                        if (r0c < 0) { r0c = 0; ++s_clamp_row; }
                        if (r1c >= (int8_t)TSP_ROWS) { r1c = (int8_t)(TSP_ROWS - 1); ++s_clamp_row; }
                        ++s_cols;
                        if (r1c >= r0c) s_cells += (unsigned)(r1c - r0c + 1);
                        if (slot >= 0) s_cells_by_step[slot] += (r1c >= r0c)
                                                              ? (unsigned)(r1c - r0c + 1) : 0;

                        /* OUTPUT the compiled sequence must supply:
                         *   slope (selects the LUT row), hl&7 (local_left),
                         *   the number of rows, and the row delta relative to
                         *   this column's own top row.  Plus the successor. */
                        an = (int16_t)(a + step);
                        phn = (unsigned)(an & 63);
                        invn = (int)((int32_t)an >> 6);
                        if (invn < 0) invn = 0; if (invn > 255) invn = 255;

                        {
                            unsigned m = 0; int16_t sm = step < 0 ? (int16_t)-step : step;
                            while (sm && m < 19) { sm >>= 1; ++m; }
                            ++s_stepmag[m];
                        }
                        /* UNCLAMPED output: what the compiled sequence would
                         * supply if screen clamping were applied separately */
                        rec_c(5, ((uint64_t)(uint16_t)step) | ((uint64_t)phase << 16)
                                 | ((uint64_t)(invl & 15u) << 24)
                                 | ((uint64_t)profile << 32),
                                 ((uint64_t)(uint8_t)slope)
                               | ((uint64_t)(hl & 7u) << 8)
                               | ((uint64_t)(uint8_t)(r1 - r0 + 1) << 16)
                               | ((uint64_t)phn_pre << 32)
                               | ((uint64_t)((unsigned)invn_pre & 15u) << 40));
                        out = ((uint64_t)(uint8_t)slope)
                            | ((uint64_t)(hl & 7u) << 8)
                            | ((uint64_t)(uint8_t)(r1c - r0c + 1) << 16)
                            | ((uint64_t)(uint8_t)(r0c - (int8_t)((TSPF_HORIZON - hl) >> 3)) << 24)
                            | ((uint64_t)phn << 32)
                            | ((uint64_t)((unsigned)invn & 15u) << 40);

                        k = ((uint64_t)(uint16_t)step)
                          | ((uint64_t)phase << 16)
                          | ((uint64_t)(invl & 15u) << 24)
                          | ((uint64_t)profile << 32)
                          | ((uint64_t)0 << 40);
                        rec_c(0, k, out);
                        rec_c(1, ((uint64_t)(uint16_t)step) | ((uint64_t)phase << 16)
                                 | ((uint64_t)invl << 24) | ((uint64_t)profile << 32), out);
                        rec_c(2, ((uint64_t)phase << 16) | ((uint64_t)(invl & 15u) << 24)
                                 | ((uint64_t)profile << 32), out);
                        rec_c(3, ((uint64_t)(uint16_t)step) | ((uint64_t)phase << 16)
                                 | ((uint64_t)profile << 32), out);
                        rec_c(4, ((uint64_t)(uint16_t)step) | ((uint64_t)(invl & 15u) << 24)
                                 | ((uint64_t)profile << 32), out);
                        seqh ^= (uint64_t)(uint8_t)slope;
                        seqh *= 1099511628211ull;
                        seqh ^= (uint64_t)(hl & 7u) | ((uint64_t)(uint8_t)(r1c - r0c + 1) << 8);
                        seqh *= 1099511628211ull;
                        ++seqlen;
                        iq = (int16_t)(iq + step);
                    }
                    if (seqlen) { seq_record(seqh, seqlen); ++s_seq_total; }
                }
            }
        }
    }

    printf("=== EDGE_PHASE: corpus provenance ===\n");
    printf("  walkable grid cells with a recipe   %lu\n", cells_walkable);
    printf("  sub-cell offsets per cell           %u  {0,0},{7,3},{3,7},{11,5} in Q4\n", n_off);
    printf("  camera positions                    %lu\n", positions);
    printf("  yaw step                            %u  -> %u headings per position\n",
           yaw_step, 256u / yaw_step);
    printf("  poses with at least one visible run %lu\n", poses);
    printf("  columns examined                    %lu\n", s_cols);
    printf("  top-edge cells emitted              %lu\n\n", s_cells);

    printf("=== distinct `step` values ===\n");
    printf("  %u distinct steps over the whole corpus\n", s_nsteps);
    {
        unsigned long tot = 0, acc = 0; unsigned k, top = 0;
        for (k = 0; k < s_nsteps; ++k) tot += s_cells_by_step[k];
        /* how many steps cover 50/90/99% of emitted cells */
        for (;;) {
            unsigned long best = 0; unsigned bi = 0; int found = 0;
            for (k = 0; k < s_nsteps; ++k)
                if (s_cells_by_step[k] > best) { best = s_cells_by_step[k]; bi = k; found = 1; }
            if (!found || !best) break;
            acc += best; s_cells_by_step[bi] = 0; ++top;
            if (top == 1) printf("  most common step covers %.1f%% of cells\n",
                                 100.0 * acc / tot);
            if (acc * 2 >= tot) { printf("  %u steps cover 50%% of cells\n", top); break; }
        }
    }

    printf("\n=== candidate minimal state: is the output DETERMINISTIC? ===\n");
    printf("  %-44s %10s %12s %10s\n", "state", "states", "observations", "conflicts");
    for (ci = 0; ci < NCAND; ++ci)
        printf("  %-44s %10lu %12lu %10lu%s\n", k_cand[ci], g_st[ci], g_ob[ci],
               g_cf[ci], g_cf[ci] ? "  <-- NOT deterministic" : "  EXACT");
    printf("\n  inverse-depth clamp fired on %lu of %lu columns (%.2f%%)\n",
           s_clamp_inv, s_cols, 100.0 * s_clamp_inv / s_cols);
    printf("  row clamp fired on           %lu of %lu columns (%.2f%%)\n",
           s_clamp_row, s_cols, 100.0 * s_clamp_row / s_cols);

    printf("\n=== |step| magnitude (the per-column Q6 inverse-depth advance) ===\n");
    {
        unsigned k; unsigned long tot = 0;
        for (k = 0; k < 20; ++k) tot += s_stepmag[k];
        for (k = 0; k < 16; ++k)
            if (s_stepmag[k])
                printf("  |step| < %-6u %10lu  %5.1f%%\n", 1u << k, s_stepmag[k],
                       100.0 * s_stepmag[k] / tot);
    }

    printf("\n=== whole-run-edge microsequence vocabulary ===\n");
    printf("  run-edges observed            %lu\n", s_seq_total);
    printf("  DISTINCT microsequences       %lu\n", g_seq_distinct);
    printf("  mean length                   %.2f columns\n",
           (double)g_seq_len_total / g_seq_distinct);
    printf("  ROM if each is stored at 2 bytes/column: %.1f KiB\n",
           2.0 * g_seq_len_total / 1024.0);
    return 0;
}
