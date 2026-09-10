/*
 * How much materializer work is thrown away by overdraw?
 *
 * The materializer is 197,878 T/update after the A24 optimisation ladder, and
 * 29.27 column-materializations happen per update on a 20-column screen. That
 * ratio implies substantial overdraw: a screen column is materialized once per
 * run that spans it, and only the nearest run's pixels survive.
 *
 * Work elimination beats per-cell tuning if the overdraw is large, so measure
 * it before building anything.
 *
 * WHY THE ORDER HAS TO FLIP
 * -------------------------
 * The host path paints far->near (`insert_run`, and A18 proved that ordering
 * is load-bearing). Under far->near the LAST writer wins, so a cell cannot be
 * skipped without knowing what a later run will do - you would have to see the
 * future. The shipped GG assembly avoids this by traversing NEAR->FAR with a
 * coverage mask, so the FIRST writer wins and every later (farther) write to
 * an owned cell is rejected.
 *
 * This probe measures both, and checks they agree:
 *
 *   1. far->near total row-writes  (what the current kernel actually does)
 *   2. near->far with a perfect coverage mask: writes that SURVIVE
 *   3. that the two produce an IDENTICAL name table
 *
 * (3) matters most. If near->far + coverage does not reproduce the far->near
 * image exactly, the saving is irrelevant because the approach is wrong.
 *
 * usage: coverage_potential_probe [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static uint16_t g_far[TSP_MAP_CELLS], g_near[TSP_MAP_CELLS];
static uint8_t g_owned[TSP_ROWS][TSP_COLS];    /* coverage mask */

static unsigned long s_writes_far = 0, s_writes_near = 0;
static unsigned long s_cols_far = 0, s_cols_near_skipped = 0;
static unsigned long s_rows_rejected = 0;
/* Split of near->far columns by how the coverage mask meets the drawn range.
 * This decides the KERNEL SHAPE: a fully-occluded column can be skipped with
 * one cheap per-column test, a fully-free column needs no per-row gating at
 * all, and only a partial column has to pay for per-cell masking. */
/* POSE-level oracle for the Z80 masked kernel. A run-scoped dump cannot
 * verify coverage: the whole point is state carried ACROSS runs, so the unit
 * of verification has to be a whole pose - every run of it, in near->far
 * order, against the final name table. */
static FILE *g_pose_dump = NULL;
static unsigned long g_pose_emitted = 0, g_pose_stride = 1, g_pose_seen = 0;

static unsigned long s_col_occluded = 0, s_col_free = 0, s_col_partial = 0;
static unsigned long s_rows_rej_occluded = 0, s_rows_rej_partial = 0;

/* Geometry for one column of a run - shared by both passes so the two differ
 * only in traversal order and masking, never in what they compute. */
typedef struct { int16_t tl, tr, bl, br; uint8_t border, shade; } ColGeom;

static void col_geom(const PolarRun *r, uint8_t profile, uint8_t c,
                     uint8_t c0, uint8_t c1, int16_t jq, int16_t step,
                     ColGeom *g)
{
    uint8_t il = (uint8_t)clamp_u8i((int16_t)((jq + 32) >> 6), 255u);
    uint8_t ir = (uint8_t)clamp_u8i((int16_t)((jq + step + 32) >> 6), 255u);
    uint8_t h0 = (uint8_t)(il >> 1), h1 = (uint8_t)(ir >> 1);
    g->tl = (int16_t)(TSPF_HORIZON - h0); g->tr = (int16_t)(TSPF_HORIZON - h1);
    g->bl = (int16_t)(TSPF_HORIZON + h0); g->br = (int16_t)(TSPF_HORIZON + h1);
    g->border = 0;
    if (profile == TSP_PROFILE_FULL) { g->tl--; g->tr--; }
    if (c == c0 && r->left_real) g->border |= 1u;
    if (c == c1 && r->right_real) g->border |= 2u;
    g->shade = g_tspf_appearance_mode
             ? shade_for((uint8_t)(((uint16_t)il + ir) >> 1),
                         k_tspf_shade_bias[r->sid]) : 1u;
    if (profile == TSP_PROFILE_LINTEL) {
        g->bl = (int16_t)(TSPF_HORIZON - (h0 >> 1));
        g->br = (int16_t)(TSPF_HORIZON - (h1 >> 1));
    } else if (profile == TSP_PROFILE_RAISED) {
        g->bl = (int16_t)(TSPF_HORIZON + h0 - (h0 >> 2));
        g->br = (int16_t)(TSPF_HORIZON + h1 - (h1 >> 2));
    } else if (profile == TSP_PROFILE_RISER) {
        g->tl = (int16_t)(TSPF_HORIZON + h0 - (h0 >> 2));
        g->tr = (int16_t)(TSPF_HORIZON + h1 - (h1 >> 2));
    }
}

/* Which rows does this column touch? Returns first/last inclusive, or 0. */
static int col_rows(const ColGeom *g, int8_t *lo, int8_t *hi)
{
    int8_t a = row_floor(g->tl < g->tr ? g->tl : g->tr);
    int8_t b = row_floor(g->bl > g->br ? g->bl : g->br);
    if (a < 0) a = 0;
    if (b >= (int8_t)TSP_ROWS) b = (int8_t)(TSP_ROWS - 1);
    if (b < a) return 0;
    *lo = a; *hi = b;
    return 1;
}

/* Same derivation as pass_run, emitted as the kernel's inputs. Returns 0 if
 * the run draws nothing (c1 < c0), in which case it is not dumped. */
static int run_params(const PolarRun *r, int *iq_o, int *step_o,
                      int *c0_o, int *c1_o, int *prof_o)
{
    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n;
    int16_t iq, step;
    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
    if (c1 < c0) return 0;
    n = (uint8_t)(c1 - c0 + 1u);
    iq = (int16_t)((int16_t)r->inv0 << 6);
    step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                     * (int16_t)k_col_recip_q8[n]);
    step = shr_signed(step, 2);
    *iq_o = iq; *step_o = step; *c0_o = c0; *c1_o = c1;
    *prof_o = k_tspf_profile[r->sid];
    return 1;
}

static void pass_run(uint16_t *out, const PolarRun *r, int near_first)
{
    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n, c;
    uint8_t profile = k_tspf_profile[r->sid];
    int16_t iq, step, jq;
    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
    if (c1 < c0) return;
    n = (uint8_t)(c1 - c0 + 1u);
    iq = (int16_t)((int16_t)r->inv0 << 6);
    step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                     * (int16_t)k_col_recip_q8[n]);
    step = shr_signed(step, 2);

    jq = iq;
    for (c = c0; c <= c1; ++c) {
        ColGeom g;
        int8_t lo, hi, rr;
        col_geom(r, profile, c, c0, c1, jq, step, &g);
        jq = (int16_t)(jq + step);

        if (near_first) {
            /* Masking must be per CELL, not per column. A near wall spanning
             * rows 8-10 does not stop a far wall spanning 5-13 from being
             * drawn - it only stops rows 8-10 of it. Skipping at column
             * granularity while still drawing the full range lets the far
             * wall overwrite the near one, which is what made the first
             * version of this probe disagree with the far->near image on 46%
             * of poses. Draw into a scratch column, then merge only the rows
             * this run actually owns. */
            static uint16_t scratch[TSP_MAP_CELLS];
            unsigned save = g_touched_count;
            int any = 0;
            if (!col_rows(&g, &lo, &hi)) continue;
            {
                int owned_any = 0;
                for (rr = lo; rr <= hi; ++rr) {
                    if (g_owned[rr][c]) owned_any = 1; else any = 1;
                }
                if (!any) {
                    ++s_cols_near_skipped; ++s_col_occluded;
                    s_rows_rejected += (hi - lo + 1);
                    s_rows_rej_occluded += (hi - lo + 1);
                    continue;
                }
                if (owned_any) ++s_col_partial; else ++s_col_free;
            }
            map_init(scratch);
            draw_edge(scratch, c, g.tl, g.tr, g.shade, 0u);
            draw_edge(scratch, c, g.bl, g.br, g.shade, 1u);
            draw_full(scratch, c, (int8_t)(row_floor(g.tl > g.tr ? g.tl : g.tr) + 1),
                      (int8_t)(row_floor(g.bl < g.br ? g.bl : g.br) - 1),
                      g.shade, g.border);
            g_touched_count = save;
            for (rr = lo; rr <= hi; ++rr) {
                if (g_owned[rr][c]) { ++s_rows_rejected; ++s_rows_rej_partial; continue; }
                out[rr * TSP_COLS + c] = scratch[rr * TSP_COLS + c];
                g_owned[rr][c] = 1;
                ++s_writes_near;
            }
            continue;
        }
        ++s_cols_far;

        draw_edge(out, c, g.tl, g.tr, g.shade, 0u);
        draw_edge(out, c, g.bl, g.br, g.shade, 1u);
        draw_full(out, c, (int8_t)(row_floor(g.tl > g.tr ? g.tl : g.tr) + 1),
                  (int8_t)(row_floor(g.bl < g.br ? g.bl : g.br) - 1),
                  g.shade, g.border);

        if (col_rows(&g, &lo, &hi)) s_writes_far += (hi - lo + 1);
    }
}

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 16u;
    if (argc > 2) {
        g_pose_dump = fopen(argv[2], "w");
        if (!g_pose_dump) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        g_pose_stride = (argc > 3) ? (unsigned long)strtoul(argv[3], 0, 0) : 8ul;
        if (!g_pose_stride) g_pose_stride = 1;
    }
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, i, c;
    unsigned long poses = 0, image_mismatch = 0;

    for (gy = 0; gy < GRID_H; ++gy) {
        for (gx = 0; gx < GRID_W; ++gx) {
            int16_t px0 = (int16_t)(gx * CELL_Q4 + 32);
            int16_t py0 = (int16_t)(gy * CELL_Q4 + 32);
            if (!tsp_is_walkable_q4(px0, py0)) continue;
            for (oi = 0; oi < n_off; ++oi) {
                int16_t px = (int16_t)(px0 + off[oi][0]);
                int16_t py = (int16_t)(py0 + off[oi][1]);
                if (!tsp_is_walkable_q4(px, py)) continue;
                for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                    uint8_t ks[64], nk = 0, count = 0, j;
                    uint8_t recipe, base_id, cond_count, lx, ly;
                    uint16_t gi, offs;
                    const uint8_t *p, *b;
                    memset(&s, 0, sizeof s);
                    s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                    gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
                    recipe = k_tspf_recipe_grid[gi];
                    if (recipe == 0xffu) continue;
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

                    /* far->near, as the host path does */
                    map_init(g_far);
                    for (i = 0; i < count; ++i)
                        pass_run(g_far, &g_runs[g_run_order[i]], 0);

                    /* near->far with a perfect coverage mask */
                    map_init(g_near);
                    memset(g_owned, 0, sizeof g_owned);
                    for (i = count; i-- > 0; )
                        pass_run(g_near, &g_runs[g_run_order[i]], 1);

                    if (g_pose_dump && (g_pose_seen++ % g_pose_stride == 0)) {
                        unsigned nd = 0, k;
                        int iqv, stv, cc0, cc1, pv;
                        for (i = count; i-- > 0; )
                            if (run_params(&g_runs[g_run_order[i]], &iqv, &stv,
                                           &cc0, &cc1, &pv)) ++nd;
                        fprintf(g_pose_dump, "%u", nd);
                        for (i = count; i-- > 0; ) {
                            const PolarRun *rr = &g_runs[g_run_order[i]];
                            if (!run_params(rr, &iqv, &stv, &cc0, &cc1, &pv))
                                continue;
                            fprintf(g_pose_dump, " %d %d %d %d %d %u %u 1",
                                    iqv, stv, cc0, cc1, pv,
                                    rr->left_real, rr->right_real);
                        }
                        for (k = 0; k < TSP_MAP_CELLS; ++k)
                            fprintf(g_pose_dump, " %u", g_near[k]);
                        fputc('\n', g_pose_dump);
                        ++g_pose_emitted;
                    }

                    for (c = 0; c < TSP_MAP_CELLS; ++c)
                        if (g_far[c] != g_near[c]) { ++image_mismatch; break; }
                    ++poses;
                }
            }
        }
    }

    printf("=== OVERDRAW: how much materializer work is thrown away? ===\n");
    printf("poses %lu\n\n", poses);
    printf("far->near (what the kernel does now):\n");
    printf("  columns materialized/update  %.2f\n", (double)s_cols_far / poses);
    printf("  row-writes/update            %.2f\n\n", (double)s_writes_far / poses);
    printf("near->far + perfect coverage mask:\n");
    printf("  columns skipped entirely     %.2f\n", (double)s_cols_near_skipped / poses);
    printf("  row-writes that SURVIVE      %.2f\n", (double)s_writes_near / poses);
    printf("  row-writes rejected by mask  %.2f\n\n", (double)s_rows_rejected / poses);
    printf("  work eliminated              %.1f%% of row-writes\n",
           100.0 * (double)s_rows_rejected
                 / (double)(s_writes_near + s_rows_rejected));
    printf("  columns eliminated           %.1f%%\n",
           100.0 * (double)s_cols_near_skipped / (double)s_cols_far);
    {
        double allc = (double)(s_col_occluded + s_col_free + s_col_partial);
        double allr = (double)(s_rows_rej_occluded + s_rows_rej_partial);
        printf("\nCOLUMN SHAPE SPLIT (near->far), what a kernel would have to pay:\n");
        printf("  fully occluded  (skip whole column)   %.2f/update  %.1f%%\n",
               (double)s_col_occluded / poses, 100.0 * s_col_occluded / allc);
        printf("  fully free      (no per-row gating)   %.2f/update  %.1f%%\n",
               (double)s_col_free / poses, 100.0 * s_col_free / allc);
        printf("  partial         (needs per-cell mask) %.2f/update  %.1f%%\n",
               (double)s_col_partial / poses, 100.0 * s_col_partial / allc);
        printf("  rejected rows from occluded columns   %.1f%% of all rejections\n",
               100.0 * s_rows_rej_occluded / allr);
        printf("  rejected rows from partial columns    %.1f%%\n",
               100.0 * s_rows_rej_partial / allr);
    }
    if (g_pose_dump) {
        fclose(g_pose_dump);
        fprintf(stderr, "pose oracle: %lu poses dumped (stride %lu)\n",
                g_pose_emitted, g_pose_stride);
    }
    printf("\nIMAGE EQUIVALENCE: %s  (%lu/%lu poses differ)\n",
           image_mismatch ? "*** MISMATCH ***" : "identical",
           image_mismatch, poses);
    if (image_mismatch)
        printf("  near->far + coverage does NOT reproduce the far->near image,\n"
               "  so the saving above is not available as stated.\n");
    return 0;
}
