/*
 * EDGE_CHAIN: is the visible boundary a small number of long chains, or many
 * short unrelated pieces?
 *
 * The persistent-walker architecture assumes that when one wall segment's
 * visible boundary ends, the next one continues it - ideally through an
 * authored vertex the two segments share, so the projected corner is reused
 * rather than recomputed.  That assumption is measurable and has never been
 * measured.
 *
 * Census, per pose:
 *   - how many endpoint projections are DUPLICATES, i.e. the same authored
 *     vertex projected twice because two visible segments share it;
 *   - screen-column ownership: who is the nearest run covering each column;
 *   - and every owner change classified as
 *        CORNER    the two owners share an authored vertex (physical corner)
 *        OCCLUSION the new owner is nearer and does not share a vertex
 *        REVEAL    the new owner is FARTHER (the foreground boundary ended)
 *        GAP       no owner on one side
 *
 * usage: edge_chain_probe [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static unsigned long s_poses, s_runs, s_proj, s_dupv;
static unsigned long s_chain_n, s_chain_len;
static unsigned long s_owner_changes, s_corner, s_occl, s_reveal, s_gap;
static unsigned long s_chainhist[24];
static unsigned long s_runs_sharing;

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 8u;
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, i, j, c;

    for (gy = 0; gy < GRID_H; ++gy)
    for (gx = 0; gx < GRID_W; ++gx) {
        int16_t px0 = (int16_t)(gx * CELL_Q4 + 32);
        int16_t py0 = (int16_t)(gy * CELL_Q4 + 32);
        if (!tsp_is_walkable_q4(px0, py0)) continue;
        for (oi = 0; oi < n_off; ++oi) {
            int16_t px = (int16_t)(px0 + off[oi][0]);
            int16_t py = (int16_t)(py0 + off[oi][1]);
            if (!tsp_is_walkable_q4(px, py)) continue;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                uint8_t ks[64], nk = 0, count = 0;
                uint8_t recipe, base_id, cond_count, lx, ly;
                uint16_t gi, offs;
                const uint8_t *p, *b;
                int owner[TSP_COLS];

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
                if (!count) continue;
                ++s_poses;
                s_runs += count;
                s_proj += 2u * count;

                /* duplicate authored vertices among the visible runs */
                for (i = 0; i < count; ++i) {
                    const PolarRun *a = &g_runs[g_run_order[i]];
                    int shares = 0;
                    for (j = 0; j < count; ++j) {
                        const PolarRun *bq = &g_runs[g_run_order[j]];
                        if (i == j) continue;
                        if (a->v0 == bq->v0 || a->v0 == bq->v1) { ++s_dupv; shares = 1; }
                        if (a->v1 == bq->v0 || a->v1 == bq->v1) { ++s_dupv; shares = 1; }
                    }
                    if (shares) ++s_runs_sharing;
                }

                /* who owns each screen column: far->near, last writer wins */
                for (c = 0; c < TSP_COLS; ++c) owner[c] = -1;
                for (i = 0; i < count; ++i) {
                    const PolarRun *r = &g_runs[g_run_order[i]];
                    unsigned a = r->x0 >> 3, e = r->x1 >> 3;
                    if (a >= TSP_COLS) a = TSP_COLS - 1u;
                    if (e >= TSP_COLS) e = TSP_COLS - 1u;
                    for (c = a; c <= e; ++c) owner[c] = (int)g_run_order[i];
                }

                /* chains of constant ownership, and what ends each one */
                {
                    unsigned len = 0;
                    for (c = 0; c < TSP_COLS; ++c) {
                        if (c && owner[c] == owner[c - 1]) { ++len; continue; }
                        if (c) {
                            ++s_chain_n; s_chain_len += len + 1u;
                            if (len + 1u < 24u) ++s_chainhist[len + 1u];
                            ++s_owner_changes;
                            if (owner[c] < 0 || owner[c - 1] < 0) ++s_gap;
                            else {
                                const PolarRun *A = &g_runs[owner[c - 1]];
                                const PolarRun *B = &g_runs[owner[c]];
                                if (A->v0 == B->v0 || A->v0 == B->v1 ||
                                    A->v1 == B->v0 || A->v1 == B->v1) ++s_corner;
                                else if (B->inv_mid > A->inv_mid) ++s_occl;
                                else ++s_reveal;
                            }
                        }
                        len = 0;
                    }
                    ++s_chain_n; s_chain_len += len + 1u;
                    if (len + 1u < 24u) ++s_chainhist[len + 1u];
                }
            }
        }
    }

    printf("=== EDGE_CHAIN: is the visible boundary one chain or many pieces? ===\n");
    printf("poses %lu   visible runs/pose %.2f\n\n",
           s_poses, (double)s_runs / s_poses);
    printf("endpoint projections/pose        %.2f\n", (double)s_proj / s_poses);
    printf("  shared-vertex INCIDENCES/pose  %.2f  (a vertex shared with two\n",
           (double)s_dupv / s_poses);
    printf("                                       other runs counts twice, so\n");
    printf("                                       this can exceed the projections)\n");
    printf("  runs sharing a vertex          %.1f%% of visible runs\n\n",
           100.0 * s_runs_sharing / s_runs);
    printf("screen-column ownership chains/pose %.2f   mean length %.2f cols\n",
           (double)s_chain_n / s_poses, (double)s_chain_len / s_chain_n);
    printf("  length histogram:");
    for (unsigned k = 1; k < 12; ++k)
        printf(" %u:%.1f%%", k, 100.0 * s_chainhist[k] / s_chain_n);
    printf("\n\nowner changes/pose %.2f, classified:\n",
           (double)s_owner_changes / s_poses);
    printf("  CORNER    shares an authored vertex   %6.2f  %5.1f%%\n",
           (double)s_corner / s_poses, 100.0 * s_corner / s_owner_changes);
    printf("  OCCLUSION nearer, no shared vertex    %6.2f  %5.1f%%\n",
           (double)s_occl / s_poses, 100.0 * s_occl / s_owner_changes);
    printf("  REVEAL    farther wall becomes owner  %6.2f  %5.1f%%\n",
           (double)s_reveal / s_poses, 100.0 * s_reveal / s_owner_changes);
    printf("  GAP       background on one side      %6.2f  %5.1f%%\n",
           (double)s_gap / s_poses, 100.0 * s_gap / s_owner_changes);
    return 0;
}
