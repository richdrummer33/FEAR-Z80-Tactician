/*
 * Measure the per-update name-table workload a span interpreter must emit.
 *
 * The interpreter's cost is dominated by how many name-table words it writes,
 * not by how many spans exist, so the cost model needs the real distribution
 * of wall cells, edge cells and background cells per rendered update -- not an
 * assumed average wall height.
 *
 * Samples the host Polar renderer (the correctness oracle) over the same pose
 * space the transition baker uses: every walkable 4-world-unit cell centre,
 * every Nth yaw. Reports per-update means and p95s.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar.h"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 8u;
    static uint16_t map[TSP_MAP_CELLS];
    TSPState s;
    uint32_t *wall = malloc(sizeof(uint32_t) * GRID_W * GRID_H * 256);
    uint32_t *edge = malloc(sizeof(uint32_t) * GRID_W * GRID_H * 256);
    uint32_t *cols = malloc(sizeof(uint32_t) * GRID_W * GRID_H * 256);
    uint32_t n = 0;
    double sum_wall = 0, sum_edge = 0, sum_cols = 0, sum_runs = 0;
    uint32_t gx, gy, yaw, i, r, c;

    if (!wall || !edge || !cols) return 1;
    tsp_polar_renderer_reset();

    for (gy = 0; gy < GRID_H; ++gy) {
        for (gx = 0; gx < GRID_W; ++gx) {
            int16_t px = (int16_t)(gx * CELL_Q4 + CELL_Q4 / 2);
            int16_t py = (int16_t)(gy * CELL_Q4 + CELL_Q4 / 2);
            if (!tsp_is_walkable_q4(px, py)) continue;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                uint32_t w = 0, e = 0, colmask = 0, runs = 0;
                memset(&s, 0, sizeof(s));
                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                tsp_polar_render(&s, map, (TSPColumn *)0);
                for (r = 0; r < TSP_ROWS; ++r) {
                    for (c = 0; c < TSP_COLS; ++c) {
                        uint16_t id = (uint16_t)(map[r * TSP_COLS + c] & TSP_TILE_ID_MASK);
                        if (id >= TSP_TILE_EDGE_BASE) { ++e; ++w; colmask |= (1u << c); }
                        else if (id >= TSP_TILE_FULL_BASE) { ++w; colmask |= (1u << c); }
                    }
                }
                /* contiguous covered-column runs: how many separate span
                 * walks the interpreter would perform across the viewport */
                for (c = 0; c < TSP_COLS; ++c)
                    if ((colmask >> c) & 1u) { if (c == 0 || !((colmask >> (c - 1)) & 1u)) ++runs; }
                wall[n] = w; edge[n] = e;
                cols[n] = __builtin_popcount(colmask);
                sum_wall += w; sum_edge += e; sum_cols += cols[n]; sum_runs += runs;
                ++n;
            }
        }
    }
    if (!n) { printf("no poses\n"); return 1; }
    qsort(wall, n, sizeof(uint32_t), cmp_u32);
    qsort(edge, n, sizeof(uint32_t), cmp_u32);
    qsort(cols, n, sizeof(uint32_t), cmp_u32);
    i = (uint32_t)((double)n * 0.95);

    printf("=== SPAN INTERPRETER WORKLOAD (host oracle, yaw_step=%u) ===\n", yaw_step);
    printf("poses sampled = %u   (viewport = %u words)\n", n, (unsigned)TSP_MAP_CELLS);
    printf("wall cells/update:       mean=%.1f  median=%u  p95=%u  max=%u\n",
           sum_wall / n, wall[n / 2], wall[i], wall[n - 1]);
    printf("  of which EDGE cells:   mean=%.1f  median=%u  p95=%u  max=%u\n",
           sum_edge / n, edge[n / 2], edge[i], edge[n - 1]);
    printf("  interior FULL cells:   mean=%.1f\n", (sum_wall - sum_edge) / n);
    printf("background cells/update: mean=%.1f\n", (double)TSP_MAP_CELLS - sum_wall / n);
    printf("covered columns/update:  mean=%.2f  median=%u  p95=%u  (of %u)\n",
           sum_cols / n, cols[n / 2], cols[i], (unsigned)TSP_COLS);
    printf("contiguous column runs:  mean=%.2f\n", sum_runs / n);
    printf("mean wall cells per covered column = %.2f\n",
           sum_wall / (sum_cols > 0 ? sum_cols : 1));
    return 0;
}
