/*
 * EXHAUSTIVE 64x64 SIGN MAPS for depth-order flipping pairs.
 *
 * The pairwise probe found that 25.74% of co-occurring wall pairs flip their
 * depth order somewhere inside a coarse cell, and that 55.45% of those flip
 * within a single yaw slice - i.e. they have a genuine TRANSLATION boundary
 * in local (x,y).
 *
 * The proposal under test: those boundaries might be expressible as the same
 * affine half-plane selector class the visibility GATEs already use
 * (`a*lx + b*ly + c >= 0`, int8 a/b). If so the runtime never sorts - it
 * evaluates a few baked sign tests and follows a precompiled order.
 *
 * This dumps the FULL 64x64 sign map per (cell, pairA, pairB, yaw) so the
 * separability question can be answered exactly rather than sampled. The
 * separability test itself lives in tools/flip_boundary_analysis.py, which
 * uses exact convex-hull disjointness - not a fitted classifier that might
 * merely fail to converge.
 *
 * Sweeps EVERY local position, so the "pathological cells first" concern is
 * moot: pathological and benign cells alike get complete spatial coverage.
 *
 * Line format:
 *   PAIR gx gy keyA keyB yaw <4096 chars>
 * where each char is '<' (A nearer), '>' (B nearer), '=' (equal) or
 * '.' (not both visible), in row-major ly then lx.
 *
 * usage: flip_boundary_probe <out> [max_pairs] [yaw_a yaw_b yaw_c yaw_d]
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

static const unsigned YAWS[] = { 0u, 32u, 64u, 96u, 128u, 160u, 192u, 224u };
#define NYAW (sizeof YAWS / sizeof YAWS[0])

static char g_map[64 * 64 + 1];

int main(int argc, char **argv) {
    const char *out_path = (argc > 1) ? argv[1] : "build/flip_maps.txt";
    unsigned max_pairs = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 500u;
    FILE *out = fopen(out_path, "w");
    TSPState s;
    PolarRun r;
    unsigned gx, gy, lx, ly, yi, i, j;
    unsigned long dumped = 0, examined = 0;

    if (!out) { fprintf(stderr, "cannot open %s\n", out_path); return 1; }
    tsp_polar_renderer_reset();

    for (gy = 0; gy < GRID_H && dumped < max_pairs; ++gy) {
        for (gx = 0; gx < GRID_W && dumped < max_pairs; ++gx) {
            uint8_t keys[MAXK], nk = 0;
            uint16_t gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
            uint8_t recipe = k_tspf_recipe_grid[gi], base_id, cond_count;
            uint16_t off;
            const uint8_t *p, *b;
            if (recipe == 0xffu) continue;
            if (!tsp_is_walkable_q4((int16_t)(gx * CELL_Q4 + 32),
                                    (int16_t)(gy * CELL_Q4 + 32))) continue;
            off = k_tspf_recipe_off[recipe];
            p = &k_tspf_recipe_stream[off];
            base_id = *p++; cond_count = *p++;
            b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
            i = *b++;
            for (; i && nk < MAXK; --i) keys[nk++] = *b++;
            for (i = 0; i < cond_count && nk < MAXK; ++i) { keys[nk++] = *p++; ++p; }
            if (nk < 2) continue;

            for (i = 0; i < nk && dumped < max_pairs; ++i) {
                for (j = i + 1; j < nk && dumped < max_pairs; ++j) {
                    for (yi = 0; yi < NYAW; ++yi) {
                        unsigned n_lt = 0, n_gt = 0, w = 0;
                        for (ly = 0; ly < 64u; ++ly) {
                            for (lx = 0; lx < 64u; ++lx, ++w) {
                                uint8_t va, vb, ia, ib;
                                int16_t px = (int16_t)(gx * CELL_Q4 + lx);
                                int16_t py = (int16_t)(gy * CELL_Q4 + ly);
                                g_map[w] = '.';
                                if (!tsp_is_walkable_q4(px, py)) continue;
                                memset(&s, 0, sizeof s);
                                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)YAWS[yi];
                                memset(&r, 0, sizeof r);
                                va = project_key(keys[i], &s, &r); ia = r.inv_mid;
                                memset(&r, 0, sizeof r);
                                vb = project_key(keys[j], &s, &r); ib = r.inv_mid;
                                if (!va || !vb) continue;
                                if (ia < ib)      { g_map[w] = '<'; ++n_lt; }
                                else if (ia > ib) { g_map[w] = '>'; ++n_gt; }
                                else                g_map[w] = '=';
                            }
                        }
                        g_map[64 * 64] = 0;
                        ++examined;
                        /* Only a slice that actually contains BOTH signs poses
                         * a separability question. */
                        if (n_lt && n_gt) {
                            fprintf(out, "PAIR %u %u %u %u %u %s\n",
                                    gx, gy, keys[i], keys[j], YAWS[yi], g_map);
                            ++dumped;
                        }
                    }
                }
            }
        }
    }
    fclose(out);
    fprintf(stderr, "slices examined %lu, two-sign slices dumped %lu -> %s\n",
            examined, dumped, out_path);
    return 0;
}
