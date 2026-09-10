/*
 * BOUNDED-ERROR temporal rendering: can the displayed frame lag the exact
 * projection by a pixel or two during motion, and buy enough deferred work to
 * be worth it?
 *
 * A29/A30/A31 measured the EXACT temporal path: every rendered frame equals
 * the perspective projection of that pose. This probe asks a different
 * question. During continuous motion, let the displayed retained state lag the
 * exact target by up to E pixels of screen-space geometric error, repairing a
 * boundary only when the error would exceed E. When motion stops, converge
 * back to exact.
 *
 * THE TRAP THIS HAS TO AVOID, STATED FIRST
 * ----------------------------------------
 * Most sub-pixel geometry change already produces no tile change at all - that
 * is A29's whole result, and it is already free. So a bounded-error scheme only
 * earns anything on transitions that WOULD have changed a tile and are
 * suppressed anyway. Measuring "cells within 1px" and calling the difference a
 * saving would double-count what A29 already banked. Everything below is
 * therefore measured against the EXACT temporal path (E=0), never against the
 * full-render baseline.
 *
 * DRIFT, AND WHY THE BOUND IS NOT AUTOMATIC
 * -----------------------------------------
 * Deferring is not free: while a boundary is held, the target keeps moving. The
 * rule is "repair when |displayed - target| > E", which bounds steady-state
 * error at E, but the error at the MOMENT of measurement can reach E plus one
 * update's motion. The probe reports the measured maximum rather than the
 * intended bound, because those are different numbers and only one of them is
 * a fact.
 *
 * CONVERGENCE
 * -----------
 * If the target stops moving while the displayed state is within E, nothing
 * ever repairs it and the image stays permanently wrong by up to E px. So the
 * rule needs a second clause: when the target has not moved since the previous
 * update, adopt it exactly. Convergence is then measured, not assumed.
 *
 * WHAT STAYS EXACT REGARDLESS OF E
 * --------------------------------
 * Topology: a span appearing or vanishing, a draw-order flip, a column
 * entering or leaving a span, and any shade or border change. A31 showed that
 * getting cross-span ownership wrong produces a small number of wrong cells on
 * most frames - the failure mode that looks almost right - so none of it is
 * allowed an error budget.
 *
 * SELF-CHECK: at E=0 this probe must reproduce the exact path cell for cell,
 * every frame, with zero measured error. That is the reduction test and it
 * runs on every invocation.
 *
 * usage: temporal_error_probe [U] [frames] [yaw_step] [regime]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"
#include "temporal_span_state.h"
#define NBUDGET 3
static const int k_budget[NBUDGET] = { 0, 1, 2 };

/* One independent simulation per error budget: each carries its own displayed
 * state and its own name table, so the budgets cannot contaminate each other. */
typedef struct {
    SpanState sp[MAXSPAN];
    uint8_t nsp;
    int live;
    /* metrics */
    unsigned long dirty, frames, deferred_cols, adopted_cols, exact_cols;
    unsigned long err_sum, err_max, err_cells_sum, err_cells_max;
    unsigned long op_defer, op_adopt, op_topo;
    unsigned long converge_frames, converge_events, converge_max;
    int settling;                   /* motion has stopped, still not exact */
    unsigned long settle_len;
    /* Of the columns this budget DEFERRED, how many would actually have
     * changed a tile in the exact path? A deferral that suppresses a
     * geometry change producing no tile change buys nothing - the exact path
     * was already free there. This is the number that says whether bounded
     * error earns its complexity. */
    unsigned long defer_would_change, defer_no_change;
    unsigned long e0_mismatch;      /* E=0 reduction check */
} Sim;

static Sim g_sim[NBUDGET];
static Pose g_target, g_prev_target;

/* ---------- pose construction (same as the exact probe) ---------- */

static void build_pose(const TSPState *st, Pose *p)
{
    uint8_t ks[64], nk = 0, count = 0, j;
    uint8_t recipe, base_id, cond_count, lx, ly, gx, gy;
    uint16_t gi, offs, i;
    const uint8_t *pp, *b;
    uint8_t run_key[MAXSPAN];

    p->nsp = 0;
    p->x_q4 = st->x_q4; p->y_q4 = st->y_q4; p->yaw = st->yaw;
    map_init(p->map);

    gx = (uint8_t)((uint16_t)st->x_q4 >> 6);
    gy = (uint8_t)((uint16_t)st->y_q4 >> 6);
    if (gx >= 48u || gy >= 24u) return;
    gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
    recipe = k_tspf_recipe_grid[gi];
    if (recipe == 0xffu) return;
    lx = (uint8_t)((uint16_t)st->x_q4 & 63u);
    ly = (uint8_t)((uint16_t)st->y_q4 & 63u);
    offs = k_tspf_recipe_off[recipe];
    pp = &k_tspf_recipe_stream[offs];
    base_id = *pp++; cond_count = *pp++;
    b = &k_tspf_base_stream[k_tspf_base_off[base_id]];
    i = *b++;
    for (; i; --i) { ks[nk] = *b++; ++nk; }
    for (i = 0; i < cond_count; ++i) {
        uint8_t key = *pp++, sel = *pp++;
        if (selector_pass(sel, lx, ly)) { ks[nk] = key; ++nk; }
    }

    tsp_polar_renderer_reset();
    g_corner_bearing_valid = 0u;
    for (j = 0; j < nk; ++j) {
        if (count >= MAXSPAN) break;
        if (!project_key(ks[j], st, &g_runs[count])) continue;
        run_key[count] = ks[j];
        insert_run(count, &count);
    }

    for (i = 0; i < count; ++i) {
        uint8_t ri = g_run_order[i];
        const PolarRun *r = &g_runs[ri];
        SpanState *s = &p->sp[p->nsp];
        uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n, c;
        uint8_t profile = k_tspf_profile[r->sid];
        int16_t iq, step, jq;

        draw_run(p->map, NULL, r);

        if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
        if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
        if (c1 < c0) continue;
        memset(s, 0, sizeof *s);
        s->keyid = run_key[ri];
        s->rank = (uint8_t)i;
        s->x0 = r->x0; s->x1 = r->x1;
        s->c0 = c0; s->c1 = c1;
        s->profile = profile;
        s->left_real = r->left_real; s->right_real = r->right_real;
        s->shade_run = 1u;
        n = (uint8_t)(c1 - c0 + 1u);
        iq = (int16_t)((int16_t)r->inv0 << 6);
        step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                         * (int16_t)k_col_recip_q8[n]);
        step = shr_signed(step, 2);
        s->iq = iq; s->step = step;
        jq = iq;
        for (c = c0; c <= c1; ++c) {
            uint8_t il = (uint8_t)clamp_u8i((int16_t)((jq + 32) >> 6), 255u);
            uint8_t ir = (uint8_t)clamp_u8i((int16_t)((jq + step + 32) >> 6), 255u);
            uint8_t hl = (uint8_t)(il >> 1), hr = (uint8_t)(ir >> 1);
            ColState *cs = &s->col[c];
            cs->present = 1u;
            cs->tl = (int16_t)(TSPF_HORIZON - hl);
            cs->tr = (int16_t)(TSPF_HORIZON - hr);
            cs->bl = (int16_t)(TSPF_HORIZON + hl);
            cs->br = (int16_t)(TSPF_HORIZON + hr);
            if (profile == TSP_PROFILE_FULL) { cs->tl--; cs->tr--; }
            if (profile == TSP_PROFILE_LINTEL) {
                cs->bl = (int16_t)(TSPF_HORIZON - (hl >> 1));
                cs->br = (int16_t)(TSPF_HORIZON - (hr >> 1));
            } else if (profile == TSP_PROFILE_RAISED) {
                cs->bl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                cs->br = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
            } else if (profile == TSP_PROFILE_RISER) {
                cs->tl = (int16_t)(TSPF_HORIZON + hl - (hl >> 2));
                cs->tr = (int16_t)(TSPF_HORIZON + hr - (hr >> 2));
            }
            cs->shade = 1u;
            cs->border = (uint8_t)(((c == c0 && r->left_real) ? 1u : 0u)
                                 | ((c == c1 && r->right_real) ? 2u : 0u));
            jq = (int16_t)(jq + step);
        }
        ++p->nsp;
    }
}

/* Render a set of span states into a name table, far->near, exactly as
 * draw_run orders its writes. This is what the viewer would see. */
static void render_state(const SpanState *sp, uint8_t nsp, uint16_t *out)
{
    int r, c; uint8_t i;
    for (r = 0; r < (int)TSP_ROWS; ++r)
        for (c = 0; c < (int)TSP_COLS; ++c) {
            uint16_t w = base_word((uint8_t)r), cw;
            for (i = 0; i < nsp; ++i)
                if (contrib(&sp[i].col[c], r, &cw)) w = cw;
            out[k_row_base[r] + c] = w;
        }
}

static SpanState *find_sp(SpanState *sp, uint8_t nsp, uint8_t keyid)
{
    uint8_t i;
    for (i = 0; i < nsp; ++i) if (sp[i].keyid == keyid) return &sp[i];
    return NULL;
}

static int col_err(const ColState *a, const ColState *b)
{
    int e = 0, d;
    d = a->tl - b->tl; if (d < 0) d = -d; if (d > e) e = d;
    d = a->tr - b->tr; if (d < 0) d = -d; if (d > e) e = d;
    d = a->bl - b->bl; if (d < 0) d = -d; if (d > e) e = d;
    d = a->br - b->br; if (d < 0) d = -d; if (d > e) e = d;
    return e;
}

/* Did the exact target move at all since the previous update? If not, motion
 * has stopped and every simulation converges to exact this frame. */
static int target_moved(const Pose *a, const Pose *b)
{
    uint8_t i; int c;
    if (a->nsp != b->nsp) return 1;
    for (i = 0; i < a->nsp; ++i) {
        if (a->sp[i].keyid != b->sp[i].keyid) return 1;
        for (c = 0; c < (int)TSP_COLS; ++c)
            if (!col_same(&a->sp[i].col[c], &b->sp[i].col[c])) return 1;
    }
    return 0;
}

/* ---------- the bounded-error step ---------- */

static uint16_t g_shown[TSP_MAP_CELLS], g_exact[TSP_MAP_CELLS];
static uint16_t g_prev_shown[TSP_MAP_CELLS];

static void step_sim(Sim *S, int budget, const Pose *tgt, int moving)
{
    uint8_t i;
    int c, r;
    unsigned long err_sum = 0, err_max = 0, bad = 0;
    SpanState next[MAXSPAN];
    uint8_t nnext = 0;

    ++S->frames;
    memcpy(g_prev_shown, g_shown, sizeof g_shown);

    /* Topology is never allowed an error budget: A31 showed that getting
     * cross-span ownership wrong costs a few cells on most frames, the failure
     * that looks almost right. Span set, order, and per-column presence all
     * come straight from the target. */
    for (i = 0; i < tgt->nsp; ++i) {
        const SpanState *t = &tgt->sp[i];
        SpanState *d = find_sp(S->sp, S->nsp, t->keyid);
        SpanState *o = &next[nnext++];
        *o = *t;                                  /* topology from the target */
        if (!d) { ++S->op_topo; continue; }       /* span appeared: exact */
        for (c = 0; c < (int)TSP_COLS; ++c) {
            const ColState *tc = &t->col[c], *dc = &d->col[c];
            int e;
            if (!tc->present) continue;
            if (!dc->present) { ++S->exact_cols; continue; }   /* new column */
            if (tc->shade != dc->shade || tc->border != dc->border) {
                ++S->exact_cols; continue;                     /* exact */
            }
            if (col_same(tc, dc)) continue;       /* already right, free */
            e = col_err(dc, tc);
            if (moving && budget && e <= budget) {
                int rr, differs = 0;
                o->col[c] = *dc;                  /* DEFER: hold the old raster */
                ++S->deferred_cols; ++S->op_defer;
                for (rr = 0; rr < (int)TSP_ROWS; ++rr) {
                    uint16_t wo, wn;
                    int ho = contrib(dc, rr, &wo), hn = contrib(tc, rr, &wn);
                    if (ho != hn || (ho && wo != wn)) { differs = 1; break; }
                }
                if (differs) ++S->defer_would_change;
                else ++S->defer_no_change;
            } else {
                ++S->adopted_cols; ++S->op_adopt;
            }
        }
    }
    memcpy(S->sp, next, sizeof(SpanState) * nnext);
    S->nsp = nnext;

    render_state(S->sp, S->nsp, g_shown);

    /* Dirty cells actually written this frame: the displayed image versus what
     * was displayed last frame. This is the quantity a kernel pays for, and it
     * is measured against the EXACT temporal path, never the full render. */
    for (c = 0; c < (int)TSP_MAP_CELLS; ++c)
        if (g_shown[c] != g_prev_shown[c]) ++S->dirty;

    /* Geometric error actually carried, and how many cells are visibly wrong. */
    for (i = 0; i < tgt->nsp; ++i) {
        const SpanState *t = &tgt->sp[i];
        const SpanState *d = find_sp(S->sp, S->nsp, t->keyid);
        if (!d) continue;
        for (c = 0; c < (int)TSP_COLS; ++c) {
            unsigned long e;
            if (!t->col[c].present || !d->col[c].present) continue;
            e = (unsigned long)col_err(&d->col[c], &t->col[c]);
            err_sum += e;
            if (e > err_max) err_max = e;
        }
    }
    for (r = 0; r < (int)TSP_MAP_CELLS; ++r)
        if (g_shown[r] != g_exact[r]) ++bad;

    S->err_sum += err_sum;
    if (err_max > S->err_max) S->err_max = err_max;
    S->err_cells_sum += bad;
    if (bad > S->err_cells_max) S->err_cells_max = bad;

    /* Convergence means: once motion STOPS, how many frames until the
     * displayed image is exact again? Counting consecutive non-exact frames
     * regardless of motion measures the length of the motion phase instead,
     * which is not the question and made this read 22-30 frames. */
    if (!moving) {
        if (bad) {
            ++S->settling;
            ++S->settle_len;
        } else if (S->settling) {
            ++S->converge_events;
            S->converge_frames += S->settle_len;
            if (S->settle_len > S->converge_max) S->converge_max = S->settle_len;
            S->settling = 0; S->settle_len = 0;
        }
    } else if (S->settling) {
        S->settling = 0; S->settle_len = 0;
    }
    if (budget == 0 && bad) ++S->e0_mismatch;
}

/* ---------- H-scroll as a first-order yaw approximation ---------- */
/*
 * A28 measured whole-COLUMN shift compensation against EXACT matching and got
 * 0.6%. This is a different test: a per-PIXEL scroll with a tolerance. Pick the
 * scroll S that best explains this update's horizontal motion, then ask how
 * many span endpoints are left within 1 or 2 px of where they should be. Under
 * a tangent projection the shift is not rigid, so the question is how much of
 * it a single global number can absorb.
 */
static unsigned long n_scroll_ends, n_scroll_resid[NBUDGET], n_scroll_pairs;
static long scroll_resid_sum, scroll_resid_max;
/* A scroll register moves the image horizontally and does nothing at all to
 * heights. Under yaw the secant term (A28) rescales every projected height, so
 * the vertical residual is whatever it is regardless of the scroll. Measure it
 * separately: this is the half a scroll cannot absorb, and it decides the
 * question. */
static unsigned long n_vert_cols, n_vert_within[NBUDGET], n_vert_sum, n_vert_max;

static void measure_scroll(const Pose *prev, const Pose *cur)
{
    int dxs[2 * MAXSPAN], nd = 0, k, best = 0;
    uint8_t i;
    long bestscore = -1;
    for (i = 0; i < cur->nsp; ++i) {
        const SpanState *p = NULL; uint8_t q;
        for (q = 0; q < prev->nsp; ++q)
            if (prev->sp[q].keyid == cur->sp[i].keyid) { p = &prev->sp[q]; break; }
        if (!p) continue;
        dxs[nd++] = (int)cur->sp[i].x0 - (int)p->x0;
        dxs[nd++] = (int)cur->sp[i].x1 - (int)p->x1;
    }
    if (!nd) return;
    ++n_scroll_pairs;
    /* Best single scroll = the one leaving the most endpoints within 1px. */
    for (k = -40; k <= 40; ++k) {
        long score = 0;
        int j;
        for (j = 0; j < nd; ++j) {
            int e = dxs[j] - k;
            if (e < 0) e = -e;
            if (e <= 1) ++score;
        }
        if (score > bestscore) { bestscore = score; best = k; }
    }
    for (k = 0; k < nd; ++k) {
        int e = dxs[k] - best, b;
        if (e < 0) e = -e;
        ++n_scroll_ends;
        scroll_resid_sum += e;
        if (e > scroll_resid_max) scroll_resid_max = e;
        for (b = 0; b < NBUDGET; ++b)
            if (e <= k_budget[b]) ++n_scroll_resid[b];
    }
    /* The vertical half, which no scroll touches. Compare each column that
     * exists in both poses AT THE SAME COLUMN INDEX - the point is what a
     * scroll leaves behind, and a scroll does not move a column vertically. */
    for (i = 0; i < cur->nsp; ++i) {
        const SpanState *p = NULL; uint8_t q; int c, b;
        for (q = 0; q < prev->nsp; ++q)
            if (prev->sp[q].keyid == cur->sp[i].keyid) { p = &prev->sp[q]; break; }
        if (!p) continue;
        for (c = 0; c < (int)TSP_COLS; ++c) {
            int e;
            if (!p->col[c].present || !cur->sp[i].col[c].present) continue;
            e = col_err(&p->col[c], &cur->sp[i].col[c]);
            ++n_vert_cols;
            n_vert_sum += (unsigned long)e;
            if ((unsigned long)e > n_vert_max) n_vert_max = (unsigned long)e;
            for (b = 0; b < NBUDGET; ++b)
                if (e <= k_budget[b]) ++n_vert_within[b];
        }
    }
}

/* ---------- corpus: identical to A29/A30 ---------- */

typedef struct { const char *name; uint8_t manual; uint8_t inp[8]; } Regime;
#define I_U TSP_INPUT_UP
#define I_L TSP_INPUT_LEFT
#define I_R TSP_INPUT_RIGHT
#define I_S TSP_INPUT_STRAFE_LEFT
static const Regime k_regime[] = {
    { "stand still",      1u, {0,0,0,0,0,0,0,0} },
    { "walk forward",     1u, {I_U,I_U,I_U,I_U,I_U,I_U,I_U,I_U} },
    { "turn in place",    1u, {I_L,I_L,I_L,I_L,I_L,I_L,I_L,I_L} },
    { "walk + turn",      1u, {I_U|I_L,I_U|I_L,I_U|I_L,I_U|I_L,
                               I_U|I_L,I_U|I_L,I_U|I_L,I_U|I_L} },
    { "strafe",           1u, {I_S,I_S,I_S,I_S,I_S,I_S,I_S,I_S} },
    { "walk, nudge turn", 1u, {I_U,I_U,I_U,I_U|I_L,I_U,I_U,I_U,I_U|I_R} },
    { "walk, rare nudge", 1u, {I_U,I_U,I_U,I_U,I_U,I_U,I_U,I_U|I_L} },
    { "demo path",        0u, {0,0,0,0,0,0,0,0} },
};
#define NREG (sizeof k_regime / sizeof k_regime[0])

/* Motion is stopped for the last quarter of every trajectory, so convergence
 * is exercised rather than assumed. Without this the probe would never see a
 * settle and could report an unconverged image as a saving. */
#define SETTLE_FRAC 4u

int main(int argc, char **argv)
{
    unsigned U = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 1u;
    unsigned frames = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 240u;
    unsigned yaw_step = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 64u;
    int only = (argc > 4) ? (int)strtol(argv[4], 0, 0) : -1;
    unsigned t, f, gx, gy, yy, b;
    unsigned long settles = 0;

    if (!U) U = 1;
    if (!yaw_step) yaw_step = 64u;
    g_tspf_appearance_mode = 0u;

    for (t = 0; t < NREG; ++t) {
        if (only >= 0 && (int)t != only) continue;
        for (gy = 0; gy < 24u; ++gy) for (gx = 0; gx < 48u; ++gx) {
            int16_t px = (int16_t)(gx * 64 + 32), py = (int16_t)(gy * 64 + 32);
            uint16_t gi;
            if (!tsp_is_walkable_q4(px, py)) continue;
            gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
            if (k_tspf_recipe_grid[gi] == 0xffu) continue;
            for (yy = 0; yy < 256u; yy += yaw_step) {
                TSPState st;
                int have_prev = 0;
                tsp_reset(&st);
                st.x_q4 = px; st.y_q4 = py;
                st.yaw = (uint8_t)yy; st.manual = k_regime[t].manual;
                for (b = 0; b < NBUDGET; ++b) {
                    g_sim[b].nsp = 0;
                    g_sim[b].settling = 0; g_sim[b].settle_len = 0;
                }
                for (f = 0; f < frames; ++f) {
                    unsigned stop = frames - frames / SETTLE_FRAC;
                    uint8_t in = (f >= stop) ? 0u
                               : k_regime[t].inp[(f / 30u) % 8u];
                    tsp_step(&st, in);
                    if ((f % U) != U - 1) continue;
                    build_pose(&st, &g_target);
                    memcpy(g_exact, g_target.map, sizeof g_exact);
                    {
                        int moving = !have_prev
                                   || target_moved(&g_target, &g_prev_target);
                        if (have_prev && !moving) ++settles;
                        if (have_prev) measure_scroll(&g_prev_target, &g_target);
                        for (b = 0; b < NBUDGET; ++b) {
                            /* each sim needs its own shown buffer restored */
                            static uint16_t keep[NBUDGET][TSP_MAP_CELLS];
                            memcpy(g_shown, keep[b], sizeof g_shown);
                            step_sim(&g_sim[b], k_budget[b], &g_target, moving);
                            memcpy(keep[b], g_shown, sizeof g_shown);
                        }
                    }
                    g_prev_target = g_target;
                    have_prev = 1;
                }
            }
        }
    }

    printf("=== BOUNDED-ERROR TEMPORAL RENDERING ===\n");
    printf("U=%u  regime %s  yaw step %u  frames %u"
           "  (last 1/%u of each trajectory is stationary)\n",
           U, only >= 0 ? k_regime[only].name : "ALL", yaw_step, frames,
           SETTLE_FRAC);
    printf("settle points seen %lu\n\n", settles);

    printf("REDUCTION TEST - at E=0 this must BE the exact path\n");
    printf("  frames where the displayed image != the exact render   %lu\n",
           g_sim[0].e0_mismatch);
    printf("  %s\n\n", g_sim[0].e0_mismatch
           ? "*** E=0 IS NOT EXACT - every number below is void ***"
           : "E=0 reduces to the exact path, as it must");

    printf("%-6s %10s %9s %9s %8s %8s %9s %9s %8s\n",
           "budget", "dirty/upd", "vs E=0", "defer%", "err mean",
           "err max", "bad cells", "bad max", "settle");
    for (b = 0; b < NBUDGET; ++b) {
        Sim *S = &g_sim[b];
        unsigned long cols = S->deferred_cols + S->adopted_cols;
        printf("%-6d %10.2f %8.1f%% %8.1f%% %8.3f %8lu %9.3f %8lu %8.2f\n",
               k_budget[b],
               (double)S->dirty / S->frames,
               g_sim[0].dirty ? 100.0 * S->dirty / g_sim[0].dirty : 0.0,
               cols ? 100.0 * S->deferred_cols / cols : 0.0,
               (double)S->err_sum / S->frames,
               S->err_max,
               (double)S->err_cells_sum / S->frames,
               S->err_cells_max,
               S->converge_events
                 ? (double)S->converge_frames / S->converge_events : 0.0);
    }
    printf("\n  dirty/upd  = cells written per update by THIS budget\n");
    printf("  vs E=0     = the same, as a %% of the exact temporal path.\n"
           "               This is the only honest saving figure: measuring\n"
           "               against the full render would re-bank what A29\n"
           "               already banked.\n");
    printf("  err max    = largest single-boundary error actually observed,\n"
           "               not the intended bound. Deferring lets the target\n"
           "               keep moving, so these differ.\n");
    printf("  settle     = frames AFTER motion stops until the image is exact\n"
           "               again. max %lu / %lu / %lu frames.\n",
           g_sim[0].converge_max, g_sim[1].converge_max, g_sim[2].converge_max);

    printf("\nWHAT DID DEFERRAL ACTUALLY SUPPRESS?\n");
    printf("  A deferred column whose geometry moved but whose TILES would not\n"
           "  have changed buys nothing: the exact path was already free there.\n");
    for (b = 1; b < NBUDGET; ++b) {
        Sim *S = &g_sim[b];
        unsigned long d = S->defer_would_change + S->defer_no_change;
        printf("  E=%d  deferred %lu columns, of which %.1f%% would have"
               " changed a tile\n", k_budget[b], d,
               d ? 100.0 * S->defer_would_change / d : 0.0);
    }

    printf("\nH-SCROLL AS A FIRST-ORDER YAW APPROXIMATION\n");
    printf("  (A28 tested whole-COLUMN shift against EXACT matching: 0.6%%."
           " This is\n   a per-PIXEL scroll with a tolerance, which is a"
           " different question.)\n");
    printf("  pose pairs %lu   span endpoints %lu\n",
           n_scroll_pairs, n_scroll_ends);
    printf("  mean |residual| after the best single scroll   %.2f px  (max %ld)\n",
           n_scroll_ends ? (double)scroll_resid_sum / n_scroll_ends : 0.0,
           scroll_resid_max);
    for (b = 0; b < NBUDGET; ++b)
        printf("  endpoints left within %d px of correct        %.1f%%\n",
               k_budget[b],
               n_scroll_ends ? 100.0 * n_scroll_resid[b] / n_scroll_ends : 0.0);
    printf("  THE HALF A SCROLL CANNOT ABSORB - vertical boundary motion,\n"
           "  same column index, %lu columns:\n", n_vert_cols);
    printf("    mean %.2f px   max %lu px\n",
           n_vert_cols ? (double)n_vert_sum / n_vert_cols : 0.0, n_vert_max);
    for (b = 0; b < NBUDGET; ++b)
        printf("    within %d px   %.1f%%\n", k_budget[b],
               n_vert_cols ? 100.0 * n_vert_within[b] / n_vert_cols : 0.0);

    return g_sim[0].e0_mismatch ? 1 : 0;
}
