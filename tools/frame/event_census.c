/* What does the scene do, expressed as boundary events on persistent surfaces?
 *
 * Every measurement so far has asked "did this thing change?" and then gone
 * looking. The alternative is to notice that we already know the old projected
 * boundary and the new one, so the event -- top edge moved up two tile rows, the
 * right end swept into a new column -- is derivable without inspecting anything.
 *
 * This censuses that. It replays a scripted camera trajectory on the host,
 * projects every key each frame exactly as the renderer does, and tracks each
 * surface across frames by its stable wall id. For every retained surface it
 * classifies the transition and counts what a swept-edge updater would have to
 * write, against what the current materializer writes for the same frame.
 *
 * The work model is deliberately crude, because that is the proposal:
 *   - a column whose top tile row moved by n pays n whole-tile writes plus one
 *     edge-tile update; FULL walls double it for the mirrored bottom
 *   - a column whose row is unchanged but whose edge word changed pays one
 *     edge-tile update (doubled for FULL)
 *   - a column that entered the span pays a full paint
 *   - a column that left the span is counted separately as a reveal, because
 *     something has to decide what is behind it
 *   - everything else pays nothing
 *
 * usage: event_census <trace.c> [frames]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

extern const uint8_t k_tsp_trace[];
extern const uint16_t k_tsp_trace_len;

#define MAXSID 32
#define ROWS   18

typedef struct {
    uint8_t live, c0, c1, profile, shade0;
    int16_t iq, step;
} Surf;

/* the renderer's depth-plane accept test, transcribed (SDCC-only in the source) */
static int plane_of(uint8_t yaw, uint8_t sid, uint8_t invd, uint8_t c0, uint8_t c1,
                    int16_t *iq_o, int16_t *step_o)
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
    *iq_o = iq; *step_o = step;
    return 1;
}

/* top-edge pixel and its tile row at one column of a surface */
static void top_at(const Surf *s, uint8_t c, int16_t *px, int8_t *row)
{
    int16_t jq = (int16_t)(s->iq + (int16_t)(c - s->c0) * s->step);
    uint8_t inv = (uint8_t)clamp_u8i((int16_t)((jq + 32) >> 6), 255u);
    uint8_t h = (uint8_t)(inv >> 1);
    int16_t t = (int16_t)(TSPF_HORIZON - h);
    if (s->profile == TSP_PROFILE_FULL) --t;
    else if (s->profile == TSP_PROFILE_RISER) t = (int16_t)(TSPF_HORIZON + h - (h >> 2));
    *px = t;
    *row = row_floor(t);
}
/* How many tile rows this surface actually covers at one column. Charging a
 * flat eighteen for an entered or vacated column overstates both the swept-edge
 * scheme and the current path; a wall only covers the rows between its top edge
 * and its bottom, and for FULL the bottom is the exact mirror. */
static unsigned covered_rows(const Surf *s, uint8_t c)
{
    int16_t px; int8_t r;
    top_at(s, c, &px, &r);
    if (r < 0) r = 0;
    if (r > 17) return 0;
    if (s->profile == TSP_PROFILE_FULL) {
        int lo = r, hi = 17 - r;
        return hi >= lo ? (unsigned)(hi - lo + 1) : 0;
    }
    return (unsigned)(18 - r);
}

/* the edge tile word this column would get, so "did the edge change" is exact */
static uint16_t edge_word_at(const Surf *s, uint8_t c)
{
    int16_t tl, tr; int8_t rl, rr;
    top_at(s, c, &tl, &rl);
    top_at(s, (uint8_t)(c + 1u <= s->c1 ? c + 1u : c), &tr, &rr);
    return edge_entry(s->shade0, (int16_t)(tl - ((int16_t)rl << 3)),
                      clamp_s8((int16_t)(tr - tl), -7, 7), 0u);
}

int main(int argc, char **argv)
{
    unsigned frames = argc > 1 ? (unsigned)strtoul(argv[1], 0, 0) : 150u;
    TSPState st;
    Surf prev[MAXSID], cur[MAXSID];
    unsigned f, i;
    /* event tallies */
    unsigned long ev_none = 0, ev_phase = 0, ev_row1 = 0, ev_rowN = 0;
    unsigned long ev_enter = 0, ev_leave = 0, ev_shade = 0;
    unsigned long surf_new = 0, surf_gone = 0, surf_kept = 0;
    /* work tallies */
    unsigned long w_fill = 0, w_edge = 0, w_paint = 0, w_reveal = 0, w_now = 0;
    unsigned long prof_hist[4] = {0,0,0,0};   /* which profiles the trace meets */

    memset(prev, 0, sizeof prev);
    tsp_reset(&st);
    tsp_polar_renderer_reset();

    for (f = 0; f < frames; ++f) {
        uint8_t ks[64], nk = 0, count = 0, j, recipe, base_id, cond_count, lx, ly;
        uint16_t gi, offs; const uint8_t *p, *b; unsigned k;
        uint8_t gx, gy;
        tsp_step(&st, k_tsp_trace[f % k_tsp_trace_len]);
        memset(cur, 0, sizeof cur);

        gx = (uint8_t)((uint16_t)st.x_q4 >> 6); gy = (uint8_t)((uint16_t)st.y_q4 >> 6);
        if (gx >= 48u || gy >= 24u) continue;
        gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
        recipe = k_tspf_recipe_grid[gi];
        if (recipe == 0xffu) continue;
        lx = (uint8_t)((uint16_t)st.x_q4 & 63u); ly = (uint8_t)((uint16_t)st.y_q4 & 63u);
        offs = k_tspf_recipe_off[recipe];
        p = &k_tspf_recipe_stream[offs];
        base_id = *p++; cond_count = *p++;
        b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
        k = *b++;
        for (; k; --k) ks[nk++] = *b++;
        for (k = 0; k < cond_count; ++k) {
            uint8_t key = *p++, sel = *p++;
            if (selector_pass(sel, lx, ly)) ks[nk++] = key;
        }
        tsp_polar_renderer_reset();
        g_corner_bearing_valid = 0u;
        for (j = 0; j < nk; ++j) {
            if (count >= TSPF_MAX_ACTIVE) break;
            if (!project_key(ks[j], &st, &g_runs[count])) continue;
            insert_run(count, &count);
        }
        for (i = 0; i < count; ++i) {
            PolarRun *r = &g_runs[g_run_order[i]];
            uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3);
            uint8_t invd; int16_t iq, step;
            if (c0 >= TSP_COLS) c0 = TSP_COLS - 1u;
            if (c1 >= TSP_COLS) c1 = TSP_COLS - 1u;
            if (c1 < c0 || r->sid >= MAXSID) continue;
            invd = inv_for_dq4(wall_d_q4(r->sid, k_tspf_seg_anchor[r->sid], &st));
            if (!plane_of(st.yaw, r->sid, invd, c0, c1, &iq, &step)) continue;
            cur[r->sid].live = 1; cur[r->sid].c0 = c0; cur[r->sid].c1 = c1;
            cur[r->sid].iq = iq; cur[r->sid].step = step;
            cur[r->sid].profile = k_tspf_profile[r->sid];
            if (cur[r->sid].profile < 4) ++prof_hist[cur[r->sid].profile];
            cur[r->sid].shade0 = 1u;
        }

        if (f) for (i = 0; i < MAXSID; ++i) {
            const Surf *a = &prev[i], *n2 = &cur[i];
            uint8_t c;
            if (!a->live && !n2->live) continue;
            if (!a->live) { ++surf_new;
                for (c = n2->c0; c <= n2->c1; ++c) { ++ev_enter; w_paint += covered_rows(n2, c); }
                continue; }
            if (!n2->live) { ++surf_gone;
                for (c = a->c0; c <= a->c1; ++c) { ++ev_leave; w_reveal += covered_rows(a, c); }
                continue; }
            ++surf_kept;
            for (c = 0; c < TSP_COLS; ++c) {
                uint8_t in_a = (c >= a->c0 && c <= a->c1), in_n = (c >= n2->c0 && c <= n2->c1);
                int16_t pa, pn; int8_t ra, rn; int d;
                uint8_t mirror = (n2->profile == TSP_PROFILE_FULL) ? 2u : 1u;
                if (in_a) w_now += covered_rows(a, c);   /* what the current path costs today */
                if (!in_a && !in_n) continue;
                if (!in_a) { ++ev_enter; w_paint += covered_rows(n2, c); continue; }
                if (!in_n) { ++ev_leave; w_reveal += covered_rows(a, c); continue; }
                top_at(a, c, &pa, &ra); top_at(n2, c, &pn, &rn);
                d = rn - ra; if (d < 0) d = -d;
                if (d == 0) {
                    if (edge_word_at(a, c) == edge_word_at(n2, c)) { ++ev_none; }
                    else { ++ev_phase; w_edge += mirror; }
                } else if (d == 1) { ++ev_row1; w_fill += mirror; w_edge += mirror; }
                else { ++ev_rowN; w_fill += (unsigned long)d * mirror; w_edge += mirror; }
            }
        }
        memcpy(prev, cur, sizeof prev);
    }

    { unsigned long ev_tot = ev_none + ev_phase + ev_row1 + ev_rowN + ev_enter + ev_leave;
      double F = frames ? (double)frames : 1.0;
      printf("event census over %u frames, %lu surface-column transitions\n\n", frames, ev_tot);
      printf("  surfaces: %lu kept, %lu appeared, %lu disappeared, per frame "
             "%.1f/%.2f/%.2f\n\n", surf_kept, surf_new, surf_gone,
             surf_kept/F, surf_new/F, surf_gone/F);
      printf("  %-34s %10s %8s %10s\n", "event", "count", "share", "per frame");
      #define E(n,v) printf("  %-34s %10lu %7.2f%% %10.2f\n", n, v, 100.0*(v)/(ev_tot?ev_tot:1), (v)/F)
      E("no boundary event at all", ev_none);
      E("edge word changed, same tile row", ev_phase);
      E("top edge crossed one tile row", ev_row1);
      E("top edge crossed several rows", ev_rowN);
      E("column entered the span", ev_enter);
      E("column left the span (a reveal)", ev_leave);
      #undef E
      printf("\n  work a frame, swept-edge updater versus the current path\n");
      printf("    whole-tile writes from swept rows   %8.1f\n", w_fill/F);
      printf("    edge-tile updates                   %8.1f\n", w_edge/F);
      printf("    full paints for entered columns     %8.1f\n", w_paint/F);
      printf("    cells needing a reveal decision     %8.1f\n", w_reveal/F);
      printf("    -------------------------------------------\n");
      printf("    swept-edge total                    %8.1f cell operations\n",
             (w_fill+w_edge+w_paint+w_reveal)/F);
      printf("    current path (every covered cell)   %8.1f cell operations\n", w_now/F);
      printf("    ratio                               %8.2fx fewer\n",
             w_now ? (double)w_now/(double)(w_fill+w_edge+w_paint+w_reveal) : 0.0);
      printf("\n  surfaces by profile (FULL is the mirrored fast path; the others move\n"
             "  their bottom edge independently and this model does NOT track that)\n    ");
      { const char *pn[4] = {"FULL","LINTEL","RAISED","RISER"}; int q;
        unsigned long pt = 0; for (q = 0; q < 4; ++q) pt += prof_hist[q];
        for (q = 0; q < 4; ++q) if (prof_hist[q])
            printf("%s %.1f%%  ", pn[q], 100.0*prof_hist[q]/(pt?pt:1)); }
      printf("\n");
    }
    return 0;
}
