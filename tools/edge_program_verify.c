/*
 * EDGE_PROGRAM_VERIFY: does the finite-program representation reproduce the
 * shipped renderer exactly, and how long are the programs actually needed?
 *
 * edge_program_bake is analytic.  This checks it against the real thing on the
 * exhaustive corpus, and measures the run-length coverage that decides which
 * horizon to bake.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static uint16_t entry_for(int phase, int r, int q, int riser) {
    int a = 8 * 1024 + phase;                /* see edge_program_bake.c */
    int step = q * 1024 + r;
    uint8_t invl = (uint8_t)clamp_u8i((int16_t)((int32_t)a >> 6), 255u);
    uint8_t invr = (uint8_t)clamp_u8i((int16_t)((int32_t)(a + step) >> 6), 255u);
    int hl = invl >> 1, hr = invr >> 1;
    int16_t tl, tr; int8_t slope, r0; uint16_t tile;
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

static unsigned long s_cols, s_tile_ok, s_adv_ok, s_clamped;
static unsigned long s_qbad, s_runs, s_cells;
static unsigned long s_runlen[40];
static long s_qmin = 99, s_qmax = -99;

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 16u;
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s; unsigned gx, gy, yaw, oi, i, j;

    for (gy = 0; gy < GRID_H; ++gy)
    for (gx = 0; gx < GRID_W; ++gx) {
        int16_t px0 = (int16_t)(gx * CELL_Q4 + 32), py0 = (int16_t)(gy * CELL_Q4 + 32);
        uint16_t gi0 = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
        if (!tsp_is_walkable_q4(px0, py0)) continue;
        if (k_tspf_recipe_grid[gi0] == 0xffu) continue;
        for (oi = 0; oi < n_off; ++oi) {
            int16_t px = (int16_t)(px0 + off[oi][0]), py = (int16_t)(py0 + off[oi][1]);
            if (!tsp_is_walkable_q4(px, py)) continue;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                uint8_t ks[64], nk = 0, count = 0, recipe, base_id, cond_count, lx, ly;
                uint16_t offs; const uint8_t *p, *b;
                memset(&s, 0, sizeof s);
                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                recipe = k_tspf_recipe_grid[gi0];
                lx = (uint8_t)((uint16_t)px & 63u); ly = (uint8_t)((uint16_t)py & 63u);
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
                tsp_polar_renderer_reset(); g_corner_bearing_valid = 0u;
                for (j = 0; j < nk; ++j) {
                    if (count >= TSPF_MAX_ACTIVE) break;
                    if (!project_key(ks[j], &s, &g_runs[count])) continue;
                    insert_run(count, &count);
                }
                if (!count) continue;

                for (i = 0; i < count; ++i) {
                    const PolarRun *rr = &g_runs[g_run_order[i]];
                    uint8_t c0 = (uint8_t)(rr->x0 >> 3), c1 = (uint8_t)(rr->x1 >> 3), c;
                    uint8_t profile = k_tspf_profile[rr->sid];
                    int riser = (profile == 3u);
                    int period = riser ? 4096 : 1024;
                    int16_t iq, step, n; int q, r;
                    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
                    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
                    if (c1 < c0) continue;
                    n = (int16_t)(c1 - c0 + 1);
                    if (rr->depth_plane) { c0 = rr->c0; c1 = rr->c1;
                                           n = (int16_t)(c1 - c0 + 1);
                                           iq = rr->iq; step = rr->step; }
                    else { iq = (int16_t)((int16_t)rr->inv0 << 6);
                           step = (int16_t)(((int16_t)rr->inv1 - (int16_t)rr->inv0)
                                            * (int16_t)k_col_recip_q8[n]);
                           step = shr_signed(step, 2); }
                    q = ((int)step) >> 10; r = ((int)step) & 1023;
                    if (q < s_qmin) s_qmin = q;
                    if (q > s_qmax) s_qmax = q;
                    if (q < -2 || q > 1) ++s_qbad;
                    ++s_runs;
                    if (n < 40) ++s_runlen[n];
                    s_cells += (unsigned)n;

                    for (c = c0; c <= c1; ++c) {
                        int a = (int)iq + 32;
                        int32_t raw_l = (int32_t)a >> 6, raw_r = (int32_t)(a + step) >> 6;
                        uint8_t invl = (uint8_t)clamp_u8i((int16_t)raw_l, 255u);
                        uint8_t invr = (uint8_t)clamp_u8i((int16_t)raw_r, 255u);
                        int hl = invl >> 1, hr = invr >> 1;
                        int16_t tl, tr; int8_t slope, r0; uint16_t tile, want, got;
                        int phase = ((a % period) + period) % period;
                        ++s_cols;
                        if (raw_l != (int32_t)invl || raw_r != (int32_t)invr) {
                            ++s_clamped; iq = (int16_t)(iq + step); continue;
                        }
                        if (riser) { tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                                     tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2)); }
                        else       { tl = (int16_t)(TSPF_HORIZON - hl);
                                     tr = (int16_t)(TSPF_HORIZON - hr); }
                        slope = clamp_s8((int16_t)(tr - tl), -7, 7);
                        r0 = row_floor(tl < tr ? tl : tr);
                        tile = edge_entry(0u, (int16_t)(tl - ((int16_t)r0 << 3)), slope, 0u);
                        want = (uint16_t)(((tile - TSP_TILE_EDGE_BASE) & 0x7f)
                             | ((uint16_t)(((hl >> 3) - (hr >> 3) + 8) & 15) << 7));
                        got = entry_for(phase, r, q, riser);
                        if ((got & 0x7f) == (want & 0x7f)) ++s_tile_ok;
                        if ((got >> 7) == (want >> 7)) ++s_adv_ok;
                        iq = (int16_t)(iq + step);
                    }
                }
            }
        }
    }

    printf("=== EDGE_PROGRAM_VERIFY ===\n");
    printf("columns checked            %lu\n", s_cols);
    printf("  inverse-depth clamped    %lu (%.3f%%, excluded - a separate path)\n",
           s_clamped, 100.0 * s_clamped / s_cols);
    {
        unsigned long n = s_cols - s_clamped;
        printf("  TILE CODE exact          %lu / %lu  (%.4f%%)\n",
               s_tile_ok, n, 100.0 * s_tile_ok / n);
        printf("  ROW ADVANCE exact        %lu / %lu  (%.4f%%)\n",
               s_adv_ok, n, 100.0 * s_adv_ok / n);
    }
    printf("  whole-tile part q range   [%ld, %ld]   outside [-2,1]: %lu\n",
           s_qmin, s_qmax, s_qbad);

    printf("\nrun length -> program horizon needed\n");
    {
        unsigned k; unsigned long acc = 0;
        static const int H[] = { 4, 8, 12, 16, 20 };
        printf("  runs %lu   columns %lu\n", s_runs, s_cells);
        for (unsigned hi = 0; hi < 5; ++hi) {
            unsigned long cells_in = 0, redis = 0;
            for (k = 1; k < 40; ++k) {
                cells_in += s_runlen[k] * (unsigned long)k;
                redis += s_runlen[k] * (unsigned long)((k + H[hi] - 1) / H[hi]);
            }
            (void)acc;
            printf("  L=%-3d dispatches/run %.2f   dispatches per 1000 cells %.1f\n",
                   H[hi], (double)redis / s_runs, 1000.0 * redis / cells_in);
        }
    }
    return 0;
}
