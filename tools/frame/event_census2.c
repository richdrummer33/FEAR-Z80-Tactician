/* Boundary-event census, corrected and driven by an explicit motion envelope.
 *
 * The first census (rung 23) reported that multi-row boundary crossings
 * essentially do not happen. That was true of the traces it ran and not safe as
 * an architectural assumption: those traces turn at MANUAL_TURN_Q4 = 48, which
 * is three yaw units an update, 4.22 degrees, only 84 degrees a second at 20 Hz.
 * The intended control envelope is 120 to 250 degrees a second, which at 20 Hz
 * is 6 to 12.5 degrees an update -- one and a half to three times faster.
 *
 * It also had two known modelling errors pulling opposite ways, both fixed here:
 *
 *   BOTTOM EDGE  it tracked only the top. That is exact for FULL, whose bottom
 *                is the mirror, and wrong for LINTEL, RAISED and RISER, which
 *                move their bottom independently and are 23-45% of surfaces.
 *   OCCLUSION    it charged surfaces hidden behind nearer ones. Runs are now
 *                walked near to far with a per-column 18-bit coverage mask, and
 *                a surface-column with no visible row raises no event at all.
 *
 * usage: event_census2 <yaw_units_per_update> <fwd_speed_q4> [frames] [label]
 *        yaw unit = 360/256 = 1.40625 degrees
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"
#include "dp_tables.h"

#define MAXSID 32
#define ROWS   18
#define HMAX   20          /* histogram ceiling for |delta| */

typedef struct {
    uint8_t live, c0, c1, profile;
    int16_t iq, step;
    uint32_t vis[TSP_COLS];    /* rows this surface actually owns, per column */
} Surf;

static int plane_of(uint8_t yaw, uint8_t sid, uint8_t invd, uint8_t c0, uint8_t c1,
                    int16_t *iq_o, int16_t *step_o)
{
    uint8_t cls = k_dp_normal_class[sid];
    int8_t nf = k_depth_nf_q7[cls][yaw], sf = k_depth_stepfac_q4[cls][yaw];
    int16_t iq = shr_signed((int16_t)((int16_t)invd * (int16_t)nf), 1);
    int16_t step = shr_signed((int16_t)((int16_t)invd * (int16_t)sf), 4);
    int16_t endq; uint8_t i, n = (uint8_t)(c1 - c0 + 1u);
    if (c0 < 10u) { for (i = c0; i < 10u; ++i) iq = (int16_t)(iq - step); }
    else          { for (i = 10u; i < c0; ++i) iq = (int16_t)(iq + step); }
    endq = iq;
    for (i = 0u; i < n; ++i) endq = (int16_t)(endq + step);
    if ((iq < 0 && endq > 0) || (iq > 0 && endq < 0)) return 0;
    if (iq < 0 || endq < 0) { iq = (int16_t)-iq; step = (int16_t)-step; }
    *iq_o = iq; *step_o = step; return 1;
}

/* Both edges at one column, exactly as draw_run derives them. */
static void edges_at(const Surf *s, uint8_t c, int16_t *top, int16_t *bot)
{
    int16_t jq = (int16_t)(s->iq + (int16_t)(c - s->c0) * s->step);
    uint8_t inv = (uint8_t)clamp_u8i((int16_t)((jq + 32) >> 6), 255u);
    uint8_t h = (uint8_t)(inv >> 1);
    int16_t t = (int16_t)(TSPF_HORIZON - h), b = (int16_t)(TSPF_HORIZON + h);
    if (s->profile == TSP_PROFILE_FULL)        { --t; }
    else if (s->profile == TSP_PROFILE_LINTEL) { b = (int16_t)(TSPF_HORIZON - (h >> 1)); }
    else if (s->profile == TSP_PROFILE_RAISED) { b = (int16_t)(TSPF_HORIZON + h - (h >> 2)); }
    else if (s->profile == TSP_PROFILE_RISER)  { t = (int16_t)(TSPF_HORIZON + h - (h >> 2)); }
    *top = t; *bot = b;
}
static uint16_t edge_word_at(const Surf *s, uint8_t c, int bottom)
{
    int16_t t0, b0, t1, b1; uint8_t c2 = (uint8_t)(c + 1u <= s->c1 ? c + 1u : c);
    edges_at(s, c, &t0, &b0); edges_at(s, c2, &t1, &b1);
    { int16_t a = bottom ? b0 : t0, e = bottom ? b1 : t1;
      int8_t r = row_floor(a);
      return edge_entry(1u, (int16_t)(a - ((int16_t)r << 3)),
                        clamp_s8((int16_t)(e - a), -7, 7), (uint8_t)bottom); }
}

static unsigned long h_top[HMAX + 1], h_bot[HMAX + 1], h_left[HMAX + 1], h_right[HMAX + 1];
static void bump(unsigned long *h, int d) { if (d < 0) d = -d; h[d > HMAX ? HMAX : d]++; }

int main(int argc, char **argv)
{
    int yawstep = argc > 1 ? atoi(argv[1]) : 3;
    int fwd     = argc > 2 ? atoi(argv[2]) : 192;
    unsigned frames = argc > 3 ? (unsigned)strtoul(argv[3], 0, 0) : 400u;
    const char *label = argc > 4 ? argv[4] : "envelope";
    TSPState st; Surf prev[MAXSID], cur[MAXSID];
    unsigned f, i; unsigned long ev_none = 0, ev_edge = 0, ev_move = 0;
    unsigned long ev_enter = 0, ev_leave = 0, ev_hidden = 0, transitions = 0;

    memset(prev, 0, sizeof prev); memset(h_top, 0, sizeof h_top);
    memset(h_bot, 0, sizeof h_bot); memset(h_left, 0, sizeof h_left);
    memset(h_right, 0, sizeof h_right);
    tsp_reset(&st); tsp_polar_renderer_reset();

    for (f = 0; f < frames; ++f) {
        uint8_t ks[64], nk = 0, count = 0, j, recipe, base_id, cond_count, lx, ly, gx, gy;
        uint16_t gi, offs; const uint8_t *p, *b; unsigned k;
        uint32_t claimed[TSP_COLS];

        /* explicit motion: a fixed yaw increment and a forward step, so the
         * envelope is the experiment's parameter rather than whatever the
         * motion model's slew happens to settle at */
        st.yaw = (uint8_t)(st.yaw + yawstep);
        { int8_t sn = (int8_t)k_tspf_sin_q7[st.yaw], cs = (int8_t)k_tspf_sin_q7[(uint8_t)(st.yaw + 64u)];
          int16_t dx = (int16_t)(((int16_t)fwd * cs) >> 11), dy = (int16_t)(((int16_t)fwd * sn) >> 11);
          if (tsp_is_walkable_q4((int16_t)(st.x_q4 + dx), st.y_q4)) st.x_q4 = (int16_t)(st.x_q4 + dx);
          if (tsp_is_walkable_q4(st.x_q4, (int16_t)(st.y_q4 + dy))) st.y_q4 = (int16_t)(st.y_q4 + dy); }

        memset(cur, 0, sizeof cur); memset(claimed, 0, sizeof claimed);
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
        tsp_polar_renderer_reset(); g_corner_bearing_valid = 0u;
        for (j = 0; j < nk; ++j) {
            if (count >= TSPF_MAX_ACTIVE) break;
            if (!project_key(ks[j], &st, &g_runs[count])) continue;
            insert_run(count, &count);
        }
        /* near to far, exactly as the renderer paints, so a hidden surface-column
         * ends up with an empty visible mask and raises no event */
        for (i = 0; i < count; ++i) {
            PolarRun *r = &g_runs[g_run_order[i]];
            uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), c;
            uint8_t invd; int16_t iq, step; Surf *s;
            if (c0 >= TSP_COLS) c0 = TSP_COLS - 1u;
            if (c1 >= TSP_COLS) c1 = TSP_COLS - 1u;
            if (c1 < c0 || r->sid >= MAXSID) continue;
            invd = inv_for_dq4(wall_d_q4(r->sid, k_tspf_seg_anchor[r->sid], &st));
            if (!plane_of(st.yaw, r->sid, invd, c0, c1, &iq, &step)) continue;
            s = &cur[r->sid];
            s->live = 1; s->c0 = c0; s->c1 = c1; s->iq = iq; s->step = step;
            s->profile = k_tspf_profile[r->sid];
            for (c = c0; c <= c1; ++c) {
                int16_t t, bo; int8_t r0, r1; uint32_t span = 0; int q;
                edges_at(s, c, &t, &bo);
                r0 = row_floor(t < bo ? t : bo); r1 = row_floor(t > bo ? t : bo);
                if (r0 < 0) r0 = 0; if (r1 > 17) r1 = 17;
                for (q = r0; q <= r1; ++q) span |= 1u << q;
                s->vis[c] = span & ~claimed[c];
                claimed[c] |= span;
            }
        }

        if (f) for (i = 0; i < MAXSID; ++i) {
            const Surf *a = &prev[i], *n2 = &cur[i]; uint8_t c;
            if (!a->live && !n2->live) continue;
            if (a->live && n2->live) {
                bump(h_left,  (int)n2->c0 - (int)a->c0);
                bump(h_right, (int)n2->c1 - (int)a->c1);
            }
            for (c = 0; c < TSP_COLS; ++c) {
                int va = a->live && a->vis[c], vn = n2->live && n2->vis[c];
                int16_t ta, ba, tn, bn; int8_t rta, rba, rtn, rbn;
                if (!va && !vn) continue;
                ++transitions;
                if (!va) { ++ev_enter; continue; }
                if (!vn) { ++ev_leave; continue; }
                edges_at(a, c, &ta, &ba); edges_at(n2, c, &tn, &bn);
                rta = row_floor(ta); rba = row_floor(ba);
                rtn = row_floor(tn); rbn = row_floor(bn);
                bump(h_top, rtn - rta); bump(h_bot, rbn - rba);
                if (rta == rtn && rba == rbn) {
                    if (edge_word_at(a, c, 0) == edge_word_at(n2, c, 0) &&
                        edge_word_at(a, c, 1) == edge_word_at(n2, c, 1)) ++ev_none;
                    else ++ev_edge;
                } else ++ev_move;
            }
            /* surface-columns suppressed entirely by nearer geometry */
            if (a->live && n2->live)
                for (c = n2->c0; c <= n2->c1; ++c) if (!n2->vis[c]) ++ev_hidden;
        }
        memcpy(prev, cur, sizeof prev);
    }

    { double T = transitions ? (double)transitions : 1.0; int q;
      printf("[%s] yaw %d units/update = %.2f deg (%.0f deg/s at 20 Hz), fwd %d\n",
             label, yawstep, yawstep * 360.0 / 256.0, yawstep * 360.0 / 256.0 * 20.0, fwd);
      printf("  %lu visible surface-column transitions over %u frames, %lu hidden suppressed\n",
             transitions, frames, ev_hidden);
      printf("    no event %.1f%%   edge word only %.1f%%   row moved %.1f%%   "
             "entered %.1f%%   left %.1f%%\n",
             100*ev_none/T, 100*ev_edge/T, 100*ev_move/T, 100*ev_enter/T, 100*ev_leave/T);
      #define H(n,h) do { unsigned long tt=0; printf("    |d %-5s|", n); \
          for (q = 0; q <= HMAX; ++q) tt += h[q]; \
          for (q = 0; q <= 6; ++q) if (h[q]) printf(" %d:%.1f%%", q, 100.0*h[q]/(tt?tt:1)); \
          { unsigned long big=0; for (q = 7; q <= HMAX; ++q) big += h[q]; \
            if (big) printf("  >=7:%.2f%%", 100.0*big/(tt?tt:1)); } printf("\n"); } while (0)
      H("top", h_top); H("bot", h_bot); H("left", h_left); H("right", h_right);
      #undef H
    }
    return 0;
}
