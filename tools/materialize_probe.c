/*
 * COLUMN MATERIALIZER oracle: the seam between the Q6 depth ramp and actual
 * name-table words.
 *
 * The Z80 kernels currently stop at (iq, step) and the emit kernel starts from
 * finished words. Nothing converts one to the other, which the code review
 * called out as "the missing column-materialization seam". This dumps the
 * shipped C's own per-column behaviour so a Z80 kernel can be built against it.
 *
 * Per column, draw_run does:
 *     invl = clamp((iq+32)>>6, 255);  invr = clamp((iq+step+32)>>6, 255)
 *     mid  = (invl+invr)>>1;  hl = invl>>1;  hr = invr>>1
 *     tl/tr = HORIZON -/+ h ... adjusted per profile
 *     border from c==c0/c1 and left_real/right_real
 *     shade = shade_for(mid, shade_bias[sid])
 *     draw_edge(top) ; draw_edge(bottom) ; draw_full(middle)
 *     iq += step
 *
 * SELF-CHECKING, like the column-solve dump
 * ------------------------------------------
 * The per-column geometry is re-derived here, which reintroduces exactly the
 * re-derivation risk this project keeps warning about - except that the
 * reconstruction is then CHECKED: every run of every pose is materialized
 * through this loop into a fresh map and compared against what
 * tsp_polar_render() produced for the same pose. If the geometry were wrong
 * the assembled name table could not match. A mismatch aborts rather than
 * emitting plausible-looking wrong rows.
 *
 * The tile EMISSION itself (draw_edge / draw_full / edge_entry) is the shipped
 * code, called directly - not reimplemented.
 *
 * Row format (one per materialized column):
 *   iq step profile border shade <18 name-table words>
 *
 * usage: materialize_probe <out> [yaw_step] [stride]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static uint16_t g_recon[TSP_MAP_CELLS];

/* draw_run's column loop, verbatim in structure, writing into `out`. */
static void materialize_run(uint16_t *out, const PolarRun *r, FILE *dump,
                            unsigned long *emitted, unsigned stride,
                            unsigned long *seen)
{
    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n, c;
    uint8_t profile = k_tspf_profile[r->sid];
    int16_t iq, step;
    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
    if (c1 < c0) return;
    n = (uint8_t)(c1 - c0 + 1u);
    iq = (int16_t)((int16_t)r->inv0 << 6);
    step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                     * (int16_t)k_col_recip_q8[n]);
    step = shr_signed(step, 2);

    for (c = c0; c <= c1; ++c) {
        uint8_t invl = (uint8_t)clamp_u8i((int16_t)((iq + 32) >> 6), 255u);
        uint8_t invr = (uint8_t)clamp_u8i((int16_t)((iq + step + 32) >> 6), 255u);
        uint8_t mid = (uint8_t)(((uint16_t)invl + invr) >> 1);
        uint8_t hl = (uint8_t)(invl >> 1), hr = (uint8_t)(invr >> 1);
        int16_t tl = (int16_t)(TSPF_HORIZON - hl), tr = (int16_t)(TSPF_HORIZON - hr);
        int16_t bl = (int16_t)(TSPF_HORIZON + hl), br = (int16_t)(TSPF_HORIZON + hr);
        uint8_t border = 0, shade;
        if (profile == TSP_PROFILE_FULL) { tl--; tr--; }
        if (c == c0 && r->left_real) border |= 1u;
        if (c == c1 && r->right_real) border |= 2u;
        shade = g_tspf_appearance_mode
              ? shade_for(mid, k_tspf_shade_bias[r->sid]) : 1u;
        if (profile == TSP_PROFILE_LINTEL) {
            bl = (int16_t)(TSPF_HORIZON - (hl >> 1));
            br = (int16_t)(TSPF_HORIZON - (hr >> 1));
        } else if (profile == TSP_PROFILE_RAISED) {
            bl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
            br = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
        } else if (profile == TSP_PROFILE_RISER) {
            tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
            tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
        }

        if (dump) {
            /* Emit this column ALONE into a scratch map so the dumped words
             * are this column's materialization, not a composite of whatever
             * earlier runs already painted. */
            static uint16_t one[TSP_MAP_CELLS];
            uint8_t rr;
            unsigned save = g_touched_count;
            map_init(one);
            draw_edge(one, c, tl, tr, shade, 0u);
            draw_edge(one, c, bl, br, shade, 1u);
            draw_full(one, c, (int8_t)(row_floor(tl > tr ? tl : tr) + 1),
                      (int8_t)(row_floor(bl < br ? bl : br) - 1), shade, border);
            g_touched_count = save;
            if ((*seen)++ % stride == 0) {
                fprintf(dump, "%d %d %u %u %u", iq, step, profile, border, shade);
                for (rr = 0; rr < TSP_ROWS; ++rr)
                    fprintf(dump, " %u", one[rr * TSP_COLS + c]);
                fputc('\n', dump);
                ++*emitted;
            }
        }

        draw_edge(out, c, tl, tr, shade, 0u);
        draw_edge(out, c, bl, br, shade, 1u);
        draw_full(out, c, (int8_t)(row_floor(tl > tr ? tl : tr) + 1),
                  (int8_t)(row_floor(bl < br ? bl : br) - 1), shade, border);
        iq = (int16_t)(iq + step);
    }
}

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "build/materialize_oracle.txt";
    unsigned yaw_step = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 16u;
    unsigned stride = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 7u;
    FILE *dump = fopen(out_path, "w");
    static uint16_t oracle[TSP_MAP_CELLS];
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, i, c;
    unsigned long poses = 0, emitted = 0, seen = 0, recon_bad = 0;

    if (!dump) { fprintf(stderr, "cannot open %s\n", out_path); return 1; }
    fprintf(stderr, "appearance_mode = %u\n", g_tspf_appearance_mode);

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
                    uint8_t ks[64], nk, count = 0, j;
                    memset(&s, 0, sizeof s);
                    s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;

                    tsp_polar_renderer_reset();
                    tsp_polar_render(&s, oracle, 0);

                    /* Rebuild the same pose through the column loop above. */
                    tsp_polar_renderer_reset();
                    map_init(g_recon);
                    {
                        uint8_t recipe, base_id, cond_count;
                        uint16_t gi = (uint16_t)(((uint16_t)gy << 5)
                                    + ((uint16_t)gy << 4) + gx), offs;
                        const uint8_t *p, *b;
                        uint8_t lx = (uint8_t)((uint16_t)px & 63u);
                        uint8_t ly = (uint8_t)((uint16_t)py & 63u);
                        recipe = k_tspf_recipe_grid[gi];
                        if (recipe == 0xffu) continue;
                        offs = k_tspf_recipe_off[recipe];
                        p = &k_tspf_recipe_stream[offs];
                        base_id = *p++; cond_count = *p++;
                        b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
                        i = *b++; nk = 0;
                        for (; i; --i) ks[nk++] = *b++;
                        for (i = 0; i < cond_count; ++i) {
                            uint8_t key = *p++, sel = *p++;
                            if (selector_pass(sel, lx, ly)) ks[nk++] = key;
                        }
                    }
                    g_corner_bearing_valid = 0u;
                    for (j = 0; j < nk; ++j) {
                        if (count >= TSPF_MAX_ACTIVE) break;
                        if (!project_key(ks[j], &s, &g_runs[count])) continue;
                        insert_run(count, &count);
                    }
                    for (i = 0; i < count; ++i)
                        materialize_run(g_recon, &g_runs[g_run_order[i]],
                                        dump, &emitted, stride, &seen);

                    for (c = 0; c < TSP_MAP_CELLS; ++c)
                        if (g_recon[c] != oracle[c]) { ++recon_bad; break; }
                    ++poses;
                }
            }
        }
    }
    fclose(dump);
    fprintf(stderr, "poses %lu, reconstruction mismatches %lu, "
                    "columns seen %lu, rows emitted %lu\n",
            poses, recon_bad, seen, emitted);
    if (recon_bad) {
        fprintf(stderr, "SELF-CHECK FAILED - the column loop does not "
                        "reproduce tsp_polar_render; dump is untrustworthy\n");
        return 2;
    }
    fprintf(stderr, "SELF-CHECK PASSED - column loop reproduces the renderer "
                    "exactly on every pose\n");
    return 0;
}
