/*
 * PAIRWISE DEPTH-ORDER FLIP MAP.
 *
 * The depth sort is now proven load-bearing: strict recipe insertion order
 * with no reordering renders only 30.55% of poses exactly. So the question
 * becomes whether the ORDER can be precompiled instead of sorted - and that
 * turns on a concrete, measurable property:
 *
 *   For each pair of walls that can co-occur in one coarse cell, is the sign
 *   of their depth difference CONSTANT over the cell, or does it FLIP?
 *
 * A pair whose sign never flips needs no runtime decision at all. A pair that
 * flips has an ownership boundary somewhere in local (x,y[,yaw]), and the
 * interesting possibility is that the boundary is an affine half-plane - the
 * same cheap selector class the visibility GATEs already use.
 *
 * THREE ORDER KEYS, MEASURED SEPARATELY - this matters
 * ----------------------------------------------------
 * The renderer sorts by `inv_mid`, which is NOT purely translational:
 *
 *     invd    = inv_for_dq4(wall_d_q4(...))     <- translation only
 *     inv0/1  = inv_at_invd(sid, invd, yawq+lo, lo)   <- adds yaw via the
 *                                                        normal dot and the
 *                                                        secant term
 *     inv_mid = (inv0 + inv1) / 2
 *
 * Perpendicular wall distance IS affine in camera position, so ordering by
 * `invd` is a translation-only property and a genuine candidate for an affine
 * selector. Ordering by `inv_mid` may not be, because yaw rescales each wall
 * differently. This probe measures both, plus inv_mid at FIXED yaw, so the
 * yaw contribution is isolated rather than assumed away.
 *
 * usage: pairwise_order_probe [xy_step] [yaw_step] [flipdump]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define MAXK 32

enum { SGN_LT = 1, SGN_GT = 2, SGN_EQ = 4 };

typedef struct {
    uint8_t seen;
    uint8_t mask_invmid;      /* over (lx,ly,yaw) */
    uint8_t mask_invd;        /* over (lx,ly) only - translation */
    uint8_t flips_at_fixed_yaw;   /* inv_mid flips within a single yaw slice */
    uint8_t flips_only_with_yaw;  /* constant per yaw slice, differs between */
} Pair;

static Pair g_pair[MAXK][MAXK];

int main(int argc, char **argv) {
    unsigned xy_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 4u;
    unsigned yaw_step = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 16u;
    FILE *fd = (argc > 3) ? fopen(argv[3], "w") : 0;
    TSPState s;
    PolarRun r;
    unsigned gx, gy, lx, ly, yaw, i, j;
    unsigned long p_total = 0, p_const = 0, p_flip = 0, p_eqonly = 0;
    unsigned long p_invd_const = 0, p_invd_flip = 0;
    unsigned long p_flip_xy = 0, p_flip_yawonly = 0;
    unsigned long cells = 0;

    tsp_polar_renderer_reset();

    for (gy = 0; gy < GRID_H; ++gy) {
        for (gx = 0; gx < GRID_W; ++gx) {
            uint8_t keys[MAXK], nk = 0;
            uint16_t gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
            uint8_t recipe = k_tspf_recipe_grid[gi], base_id, cond_count;
            uint16_t off;
            const uint8_t *p, *b;
            if (recipe == 0xffu) continue;
            if (!tsp_is_walkable_q4((int16_t)(gx * CELL_Q4 + 32),
                                    (int16_t)(gy * CELL_Q4 + 32))) continue;

            /* Full MEMBERSHIP set for the cell: base keys plus every
             * conditional key regardless of its selector. Ordering must be
             * decidable for any pair that CAN co-occur, not just those the
             * selectors happen to admit at one sampled position. */
            off = k_tspf_recipe_off[recipe];
            p = &k_tspf_recipe_stream[off];
            base_id = *p++; cond_count = *p++;
            b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
            i = *b++;
            for (; i && nk < MAXK; --i) keys[nk++] = *b++;
            for (i = 0; i < cond_count && nk < MAXK; ++i) { keys[nk++] = *p++; ++p; }
            if (nk < 2) continue;
            ++cells;

            memset(g_pair, 0, sizeof g_pair);

            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                static uint8_t slice[MAXK][MAXK];   /* sign mask this yaw only */
                memset(slice, 0, sizeof slice);
                for (ly = 0; ly < 64u; ly += xy_step) {
                    for (lx = 0; lx < 64u; lx += xy_step) {
                        uint8_t vis[MAXK], im[MAXK], id[MAXK];
                        int16_t px = (int16_t)(gx * CELL_Q4 + lx);
                        int16_t py = (int16_t)(gy * CELL_Q4 + ly);
                        if (!tsp_is_walkable_q4(px, py)) continue;
                        memset(&s, 0, sizeof s);
                        s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                        for (i = 0; i < nk; ++i) {
                            memset(&r, 0, sizeof r);
                            vis[i] = project_key(keys[i], &s, &r);
                            im[i] = r.inv_mid;
                            id[i] = inv_for_dq4(wall_d_q4(
                                (uint8_t)(k_tspf_keys[keys[i]] & 31u),
                                k_tspf_seg_anchor[k_tspf_keys[keys[i]] & 31u], &s));
                        }
                        for (i = 0; i < nk; ++i) {
                            if (!vis[i]) continue;
                            for (j = i + 1; j < nk; ++j) {
                                uint8_t m;
                                if (!vis[j]) continue;
                                g_pair[i][j].seen = 1;
                                m = (im[i] < im[j]) ? SGN_LT
                                  : (im[i] > im[j]) ? SGN_GT : SGN_EQ;
                                g_pair[i][j].mask_invmid |= m;
                                slice[i][j] |= m;
                                m = (id[i] < id[j]) ? SGN_LT
                                  : (id[i] > id[j]) ? SGN_GT : SGN_EQ;
                                g_pair[i][j].mask_invd |= m;
                            }
                        }
                    }
                }
                /* Did inv_mid order flip WITHIN this single yaw slice? */
                for (i = 0; i < nk; ++i)
                    for (j = i + 1; j < nk; ++j) {
                        uint8_t m = slice[i][j] & (SGN_LT | SGN_GT);
                        if (m == (SGN_LT | SGN_GT)) g_pair[i][j].flips_at_fixed_yaw = 1;
                    }
            }

            for (i = 0; i < nk; ++i) {
                for (j = i + 1; j < nk; ++j) {
                    Pair *pp = &g_pair[i][j];
                    uint8_t hard, hardd;
                    if (!pp->seen) continue;
                    ++p_total;
                    hard = pp->mask_invmid & (SGN_LT | SGN_GT);
                    hardd = pp->mask_invd & (SGN_LT | SGN_GT);
                    if (hard == 0) { ++p_eqonly; continue; }
                    if (hard == (SGN_LT | SGN_GT)) {
                        ++p_flip;
                        if (pp->flips_at_fixed_yaw) ++p_flip_xy;
                        else { ++p_flip_yawonly; pp->flips_only_with_yaw = 1; }
                        if (fd) fprintf(fd, "FLIP %u %u %u %u %u\n", gx, gy,
                                        keys[i], keys[j], pp->flips_at_fixed_yaw);
                    } else ++p_const;
                    if (hardd == (SGN_LT | SGN_GT)) ++p_invd_flip;
                    else ++p_invd_const;
                }
            }
        }
    }
    if (fd) fclose(fd);

    printf("=== PAIRWISE DEPTH-ORDER FLIP MAP ===\n");
    printf("cells with >=2 candidates   %lu   (lx,ly step %u; yaw step %u)\n\n",
           cells, xy_step, yaw_step);
    printf("co-occurring wall pairs      %lu\n", p_total);
    printf("  order CONSTANT over cell   %lu   (%.2f%%)  <- need no runtime decision\n",
           p_const, 100.0 * (double)p_const / (double)p_total);
    printf("  always EQUAL depth         %lu   (%.2f%%)  <- tie; convention decides\n",
           p_eqonly, 100.0 * (double)p_eqonly / (double)p_total);
    printf("  order FLIPS                %lu   (%.2f%%)  <- real ownership boundary\n\n",
           p_flip, 100.0 * (double)p_flip / (double)p_total);
    printf("of the flipping pairs:\n");
    printf("  flip within a fixed yaw (translation boundary)  %lu   (%.2f%%)\n",
           p_flip_xy, 100.0 * (double)p_flip_xy / (double)(p_flip ? p_flip : 1));
    printf("  constant per yaw, differ between yaws           %lu   (%.2f%%)\n",
           p_flip_yawonly, 100.0 * (double)p_flip_yawonly / (double)(p_flip ? p_flip : 1));
    printf("     ^ these have NO translation boundary; an affine selector in\n");
    printf("       (lx,ly) alone cannot express them - they are yaw events.\n\n");
    printf("ordering by invd instead (perpendicular distance, TRANSLATION ONLY):\n");
    printf("  constant over cell         %lu   (%.2f%%)\n",
           p_invd_const, 100.0 * (double)p_invd_const / (double)p_total);
    printf("  flips                      %lu   (%.2f%%)\n",
           p_invd_flip, 100.0 * (double)p_invd_flip / (double)p_total);
    return 0;
}
