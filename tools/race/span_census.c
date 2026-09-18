/* What span lengths does the renderer actually produce?
 *
 * The race measured 3, 6, 12 and 18 columns because those are tidy multiples of
 * the six-column chunk. The selector's weakest case is a short span, where the
 * per-span setup -- two bank switches, the map read, the base formation -- is
 * amortised over one chunk or less. Whether that weakness matters is a question
 * about the renderer, not about the kernel, so it has to be measured against
 * real poses rather than assumed.
 *
 * This walks the same pose space the coverage probe walks: every walkable map
 * cell at four sub-cell offsets and a yaw sweep, projecting every key the recipe
 * grid selects, and histogramming the spans that survive projection.
 *
 * It reports, for every span the renderer would hand the raster kernel:
 *   - the length distribution in columns
 *   - the chunk decomposition: how many six-column chunks, and the terminal one
 *   - the share of spans whose WHOLE length is a short terminal chunk
 *   - the share of total COLUMN work those spans represent, which is the number
 *     that decides whether specialising short spans is worth anything
 *
 * usage: span_census [yaw_step]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"
#include "dp_tables.h"   /* the host-side copy of the baked depth-plane tables */

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64
#define MAXCOL 24

static FILE *g_dump = NULL;

/* The renderer's depth-plane path is compiled only under SDCC, so on the host
 * r->depth_plane is always zero and a naive census would report that no span
 * uses the selector at all. This is a transcription of screen_depth_plane's
 * accept/reject decision -- NOT a second implementation of the geometry, just
 * the test for whether the signed wall plane crosses zero inside the run, which
 * is the only thing that sends a span to the endpoint fallback instead. */
static int plane_accepts(uint8_t yaw, uint8_t sid, uint8_t invd, uint8_t c0, uint8_t c1,
                         int *step_o, int *iq_o)
{
    uint8_t cls = k_dp_normal_class[sid];
    int8_t nf = k_depth_nf_q7[cls][yaw], sf = k_depth_stepfac_q4[cls][yaw];
    int16_t iq = shr_signed((int16_t)((int16_t)invd * (int16_t)nf), 1);
    int16_t step = shr_signed((int16_t)((int16_t)invd * (int16_t)sf), 4);
    int16_t endq;
    uint8_t i, n = (uint8_t)(c1 - c0 + 1u);
    if (c0 < 10u) { for (i = c0; i < 10u; ++i) iq = (int16_t)(iq - step); }
    else          { for (i = 10u; i < c0; ++i) iq = (int16_t)(iq + step); }
    endq = iq;
    for (i = 0u; i < n; ++i) endq = (int16_t)(endq + step);
    if ((iq < 0 && endq > 0) || (iq > 0 && endq < 0)) return 0;
    if (iq < 0 || endq < 0) { iq = (int16_t)-iq; step = (int16_t)-step; }
    *step_o = step; *iq_o = iq;
    return 1;
}

int main(int argc, char **argv)
{
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 4u;
    if (argc > 2) {
        g_dump = fopen(argv[2], "w");
        if (!g_dump) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
        fprintf(g_dump, "iq,step,cols\n");
    }
    static const int8_t off[][2] = { {0,0},{7,3},{3,7},{11,5} };
    const unsigned n_off = sizeof off / sizeof off[0];
    TSPState s;
    unsigned gx, gy, yaw, oi, i;
    unsigned long poses = 0, spans = 0, cols = 0;
    unsigned long hist[MAXCOL + 1];
    unsigned long tail[7];              /* terminal chunk length, 1..6 */
    unsigned long chunks_full = 0, chunks_tail = 0;
    unsigned long cols_in_short = 0;    /* columns belonging to spans <= 3 cols */
    unsigned long spans_short = 0;
    unsigned long steps_seen = 0, plane_spans = 0, stepbig = 0;
    unsigned long dumped = 0, dump_oor = 0;

    memset(hist, 0, sizeof hist);
    memset(tail, 0, sizeof tail);

    for (gy = 0; gy < GRID_H; ++gy) for (gx = 0; gx < GRID_W; ++gx) {
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
                ++poses;

                for (i = 0; i < count; ++i) {
                    const PolarRun *r = &g_runs[g_run_order[i]];
                    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3);
                    unsigned n, nf, tl;
                    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1u;
                    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1u;
                    if (c1 < c0) continue;
                    n = (unsigned)(c1 - c0 + 1u);
                    if (n > MAXCOL) n = MAXCOL;
                    ++spans; cols += n; ++hist[n];
                    { int st = 0, iq = 0;
                      uint8_t invd = inv_for_dq4(wall_d_q4(r->sid, k_tspf_seg_anchor[r->sid], &s));
                      if (plane_accepts((uint8_t)yaw, r->sid, invd, c0, c1, &st, &iq)) {
                          int mag = st < 0 ? -st : st;
                          ++plane_spans; steps_seen += (unsigned long)mag;
                          if (mag > 2047) ++stepbig;
                          /* The tuple the raster walker is actually handed. Weighting
                           * by span length alone is not enough: the linear scan's cost
                           * depends on WHICH band the phase lands in, and the replay's
                           * cost on how many move bytes the body expands to. Both vary
                           * with (step, phase), so the honest projection replays the
                           * real tuples rather than sampling step and phase uniformly. */
                      { int cc; unsigned ok = 1;
                        for (cc = 0; cc <= (int)n; ++cc) {
                            long a = (long)iq + (long)cc * st + 32;
                            if (a < 0 || (a >> 6) > 255) { ok = 0; break; }
                        }
                        if (ok) { ++dumped; if (g_dump) fprintf(g_dump, "%d,%d,%u\n", iq, st, n); }
                        else ++dump_oor; }
                      } }
                    nf = (n - 1u) / 6u;         /* whole six-column chunks before the last */
                    tl = n - nf * 6u;           /* the terminal chunk, 1..6 */
                    chunks_full += nf; ++chunks_tail; ++tail[tl];
                    if (n <= 3u) { ++spans_short; cols_in_short += n; }
                }
            }
        }
    }

    printf("span census over %lu poses (yaw step %u)\n", poses, yaw_step);
    printf("  %lu spans, %lu columns, %.2f spans per pose, %.2f columns per span\n\n",
           spans, cols, (double)spans / poses, (double)cols / spans);

    printf("  span length distribution\n");
    for (i = 1; i <= MAXCOL; ++i) if (hist[i])
        printf("    %2u cols  %8lu  %5.2f%%  (%5.2f%% of columns)\n",
               i, hist[i], 100.0 * hist[i] / spans, 100.0 * hist[i] * i / cols);

    printf("\n  chunk decomposition: %lu full six-column chunks, %lu terminal chunks\n",
           chunks_full, chunks_tail);
    printf("  terminal chunk length\n");
    for (i = 1; i <= 6; ++i) if (tail[i])
        printf("    %u cols  %8lu  %5.2f%% of spans\n", i, tail[i], 100.0 * tail[i] / spans);

    printf("\n  spans of three columns or fewer: %lu (%.2f%% of spans) carrying\n",
           spans_short, 100.0 * spans_short / spans);
    printf("  %lu columns (%.2f%% of the column work)\n",
           cols_in_short, 100.0 * cols_in_short / cols);
    printf("  %lu of %lu spans (%.2f%%) take the depth-plane path the selector needs;\n",
           plane_spans, spans, 100.0 * plane_spans / spans);
    printf("  the rest fall back to the endpoint path because the wall plane crosses\n");
    printf("  zero inside the run. Mean |step| on the accepted spans: %.1f, %lu over 2047\n",
           plane_spans ? (double)steps_seen / plane_spans : 0.0, stepbig);

    /* the cost model, weighted by what the renderer actually produces */
    {
        FILE *f = fopen("build/race/span_weights.csv", "w");
        if (f) { fprintf(f, "cols,spans,columns\n");
                 for (i = 1; i <= MAXCOL; ++i) if (hist[i])
                     fprintf(f, "%u,%lu,%lu\n", i, hist[i], hist[i] * i);
                 fclose(f);
                 printf("\n  weights written to build/race/span_weights.csv\n"); }
    }
    if (g_dump) fclose(g_dump);
    printf("\n  THE CLAMP. tilesector_polar_renderer.c:815 clamps the inverse depth to\n");
    printf("  0..255 per column before deriving the row. The raced DDA and the baked\n");
    printf("  bodies both implement the UNCLAMPED rule, so wherever a column clamps the\n");
    printf("  selector and the shipping raster are different functions.\n");
    printf("    %lu of %lu accepted spans (%.2f%%) stay inside the unclamped domain\n",
           dumped, plane_spans, 100.0 * dumped / plane_spans);
    printf("    %lu (%.2f%%) touch the clamp on at least one column\n",
           dump_oor, 100.0 * dump_oor / plane_spans);
    printf("    selector-eligible share of all spans: %.2f%%\n",
           100.0 * dumped / spans);
    printf("  Detecting it is two compares on the span endpoints, because the step has a\n");
    printf("  fixed sign, so routing those spans to the existing path is cheap. This has\n");
    printf("  to be part of integration; it is not optional.\n");
    return 0;
}
