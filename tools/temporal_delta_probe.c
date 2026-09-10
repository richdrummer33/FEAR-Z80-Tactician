/*
 * How much does the screen actually CHANGE between updates?
 *
 * A25/A26/A27 closed spatial coverage: dedup within one frame caps at 9.1% of
 * row-writes and loses on both kernels once the bookkeeping is paid for. The
 * remaining idea in that family is temporal - skip materializing cells that
 * did not change since the previous update. That has a different ceiling and
 * nothing had measured it. This measures it before anything is built, which is
 * the same order that saved two wasted kernels in A25 and A26.
 *
 * WHAT DECIDES THE CEILING, AND WHY IT IS NOT OBVIOUS
 * --------------------------------------------------
 * The pose does not move by "a little" between updates. The materializer costs
 * 194,761 T against a 59,736 T frame, so an update lands roughly every 4th
 * frame while `tsp_step` runs the motion model EVERY frame. Four frames of
 * motion is not a small delta, and a yaw delta is the expensive kind: the
 * projection maps +/-512 q12 of bearing onto 160 pixels, so one yaw unit
 * (16 q12) is 2 to 3.5 screen pixels depending where on the screen it lands.
 * A handful of yaw units per update slides the whole image across several tile
 * columns.
 *
 * So this probe reports the delta as a FUNCTION of the update period, not as
 * one number. If the ceiling only exists at U=1 it is not a ceiling this
 * project can reach.
 *
 * WHAT IS MEASURED
 * ----------------
 * The materializer's unit of work is one (run, screen column), and that unit's
 * entire output is a pure function of
 *
 *     K = (profile, il, ir, border, shade)
 *
 * where il/ir are the QUANTIZED inverse depths at the column's two edges,
 * il = clamp((jq+32)>>6). Quantization is the whole reason a temporal skip
 * could work at all: jq moves continuously, il does not.
 *
 *   M1  cell stability     - of 360 name-table words, how many equal the
 *                            previous update's. Upper bound on eliminable
 *                            STORES, and nothing more: it does not say the
 *                            work that derived them can be skipped.
 *   M2  column stability   - screen columns whose 18 words are all unchanged.
 *   M3  work-unit stability- run-columns whose K is unchanged from the same
 *                            (sid, column) last update.
 *   M4  skippable columns  - screen columns where the ORDERED list of (sid, K)
 *                            is identical to last update. These are provably
 *                            skippable in full: same runs, same order, same
 *                            per-column output, so the same final words. This
 *                            is the number that maps onto the 194,761 T.
 *
 * M4 <= M2 is a hard invariant - an identical run list must produce identical
 * cells - and the probe ASSERTS it rather than trusting it. The gap M2 - M4 is
 * meaningful on its own: those are columns whose pixels coincide even though
 * the geometry changed, which a key test cannot see and only a post-hoc cell
 * compare could, i.e. after the work is already done.
 *
 * usage: temporal_delta_probe [frames_per_update] [frames_per_traj]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define MAX_RUNCOLS 512u
#define NBUCKET 6u

/* One materializer work unit, in the form that decides its output exactly. */
typedef struct { uint8_t sid, col, profile, il, ir, border, shade;
                 int8_t lo, hi; uint16_t word[TSP_ROWS]; } WorkKey;

/* Per-RUN parameters, kept so a changed work key can be attributed: did the
 * run's projected geometry move, or only the grid it is resampled onto? */
typedef struct { uint8_t sid, c0, c1, inv0, inv1, left_real, right_real;
                 int16_t step; } RunParam;

typedef struct {
    uint16_t map[TSP_MAP_CELLS];
    RunParam rp[TSPF_MAX_ACTIVE];
    uint8_t nrp;
    WorkKey wk[MAX_RUNCOLS];
    uint16_t nwk;
    uint8_t  col_first[TSP_COLS];   /* index of first work unit in column c */
    uint8_t  col_n[TSP_COLS];       /* how many work units in column c */
    int16_t  x_q4, y_q4;
    uint8_t  yaw;
} Snapshot;

static Snapshot g_a, g_b;
static uint16_t g_scratch[TSP_MAP_CELLS];

/* Same derivation as draw_run's column loop, in appearance mode 0, reduced to
 * the fields that determine the column's output words. Shared by nothing else
 * on purpose: if this drifts from draw_run the M4<=M2 assertion fires. */
static void run_workkeys(const PolarRun *r, Snapshot *s)
{
    uint8_t c0 = (uint8_t)(r->x0 >> 3), c1 = (uint8_t)(r->x1 >> 3), n, c;
    uint8_t profile = k_tspf_profile[r->sid];
    int16_t iq, step, jq;
    if (c0 >= TSP_COLS) c0 = TSP_COLS - 1;
    if (c1 >= TSP_COLS) c1 = TSP_COLS - 1;
    if (c1 < c0) return;
    n = (uint8_t)(c1 - c0 + 1u);
    iq = (int16_t)((int16_t)r->inv0 << 6);
    step = (int16_t)(((int16_t)r->inv1 - (int16_t)r->inv0)
                     * (int16_t)k_col_recip_q8[n]);
    step = shr_signed(step, 2);

    if (s->nrp < TSPF_MAX_ACTIVE) {
        RunParam *rp = &s->rp[s->nrp++];
        rp->sid = r->sid; rp->c0 = c0; rp->c1 = c1;
        rp->inv0 = r->inv0; rp->inv1 = r->inv1;
        rp->left_real = r->left_real; rp->right_real = r->right_real;
        rp->step = step;
    }

    jq = iq;
    for (c = c0; c <= c1; ++c) {
        WorkKey *w;
        if (s->nwk >= MAX_RUNCOLS) return;
        w = &s->wk[s->nwk++];
        w->sid = r->sid;
        w->col = c;
        w->profile = profile;
        w->il = (uint8_t)clamp_u8i((int16_t)((jq + 32) >> 6), 255u);
        w->ir = (uint8_t)clamp_u8i((int16_t)((jq + step + 32) >> 6), 255u);
        w->border = (uint8_t)(((c == c0 && r->left_real) ? 1u : 0u)
                            | ((c == c1 && r->right_real) ? 2u : 0u));
        w->shade = g_tspf_appearance_mode
                 ? shade_for((uint8_t)(((uint16_t)w->il + w->ir) >> 1),
                             k_tspf_shade_bias[r->sid]) : 1u;

        /* Materialize this column on its own so its row range and the exact
         * words it stores are recoverable. Same shape as A25's scratch column;
         * g_touched_count is saved and restored because the scratch draw must
         * not pollute the retained-background bookkeeping. */
        {
            uint8_t hl = (uint8_t)(w->il >> 1), hr = (uint8_t)(w->ir >> 1);
            int16_t tl = (int16_t)(TSPF_HORIZON - hl), tr = (int16_t)(TSPF_HORIZON - hr);
            int16_t bl = (int16_t)(TSPF_HORIZON + hl), br = (int16_t)(TSPF_HORIZON + hr);
            int8_t a, bq, rr;
            /* The scratch draw must leave the renderer's retained-background
             * bookkeeping exactly as it found it. map_init() on the scratch
             * would clear g_touched_bits while g_touched_count keeps counting,
             * so every later mark_touched re-appends cells already in the list
             * and g_touched_list[TSP_MAP_CELLS] overruns into neighbouring
             * statics. Caught by the run-list/cells invariant, which went from
             * 0 to 9 violations the moment the scratch draw was added. */
            uint8_t save_bits[sizeof g_touched_bits];
            uint16_t save_count = g_touched_count;
            uint8_t save_ready = g_map_ready;
            memcpy(save_bits, g_touched_bits, sizeof g_touched_bits);
            if (profile == TSP_PROFILE_FULL) { tl--; tr--; }
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
            a = row_floor(tl < tr ? tl : tr);
            bq = row_floor(bl > br ? bl : br);
            if (a < 0) a = 0;
            if (bq >= (int8_t)TSP_ROWS) bq = (int8_t)(TSP_ROWS - 1);
            if (bq < a) { w->lo = 1; w->hi = 0; }
            else {
                for (rr = 0; rr < (int8_t)TSP_ROWS; ++rr)
                    g_scratch[k_row_base[rr] + c] = base_word((uint8_t)rr);
                draw_edge(g_scratch, c, tl, tr, w->shade, 0u);
                draw_edge(g_scratch, c, bl, br, w->shade, 1u);
                draw_full(g_scratch, c,
                          (int8_t)(row_floor(tl > tr ? tl : tr) + 1),
                          (int8_t)(row_floor(bl < br ? bl : br) - 1),
                          w->shade, w->border);
                w->lo = a; w->hi = bq;
                for (rr = a; rr <= bq; ++rr)
                    w->word[rr] = g_scratch[k_row_base[rr] + c];
            }
            memcpy(g_touched_bits, save_bits, sizeof g_touched_bits);
            g_touched_count = save_count;
            g_map_ready = save_ready;
        }
        jq = (int16_t)(jq + step);
    }
}

/* Rebuild the pose's run list exactly as tsp_polar_render does, then record
 * both the final name table and the work keys, far->near (draw order). */
static void snapshot(const TSPState *st, Snapshot *s)
{
    uint8_t ks[64], nk = 0, count = 0, j;
    uint8_t recipe, base_id, cond_count, lx, ly, gx, gy, c;
    uint16_t gi, offs, i;
    const uint8_t *p, *b;

    s->nwk = 0;
    s->nrp = 0;
    s->x_q4 = st->x_q4; s->y_q4 = st->y_q4; s->yaw = st->yaw;
    memset(s->col_n, 0, sizeof s->col_n);
    map_init(s->map);

    gx = (uint8_t)((uint16_t)st->x_q4 >> 6);
    gy = (uint8_t)((uint16_t)st->y_q4 >> 6);
    if (gx >= 48u || gy >= 24u) return;
    gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
    recipe = k_tspf_recipe_grid[gi];
    if (recipe == 0xffu) return;
    lx = (uint8_t)((uint16_t)st->x_q4 & 63u);
    ly = (uint8_t)((uint16_t)st->y_q4 & 63u);
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
        if (!project_key(ks[j], st, &g_runs[count])) continue;
        insert_run(count, &count);
    }

    /* far->near, exactly the shipped draw order */
    for (i = 0; i < count; ++i) {
        draw_run(s->map, NULL, &g_runs[g_run_order[i]]);
        run_workkeys(&g_runs[g_run_order[i]], s);
    }

    /* Index the work units by column. They are appended run by run, so a
     * column's units are NOT contiguous; sort by column, stable in draw order,
     * so the per-column list can be compared as a sequence. */
    {
        WorkKey tmp[MAX_RUNCOLS];
        uint16_t k = 0, q;
        for (c = 0; c < TSP_COLS; ++c) {
            s->col_first[c] = 0;
            for (q = 0; q < s->nwk; ++q)
                if (s->wk[q].col == c) {
                    if (s->col_n[c] == 0) s->col_first[c] = (uint8_t)k;
                    tmp[k++] = s->wk[q];
                    ++s->col_n[c];
                }
        }
        memcpy(s->wk, tmp, sizeof(WorkKey) * k);
        s->nwk = k;
    }
}

static int wk_eq(const WorkKey *a, const WorkKey *b)
{
    return a->sid == b->sid && a->profile == b->profile && a->il == b->il
        && a->ir == b->ir && a->border == b->border && a->shade == b->shade;
}

/* Accumulators, split by |dyaw| bucket so the answer is a curve, not a point. */
static unsigned long n_pairs[NBUCKET], n_cells_same[NBUCKET];
static unsigned long n_cols_same[NBUCKET], n_cols_skippable[NBUCKET];
static unsigned long n_cols_live[NBUCKET], n_cols_skip_live[NBUCKET];
static unsigned long n_wk[NBUCKET], n_wk_same[NBUCKET];
static unsigned long n_wk_in_skippable[NBUCKET];
static unsigned long sum_dyaw[NBUCKET], sum_dpos[NBUCKET];
static unsigned long invariant_violations = 0;

/* Yaw-compensated variants. Rotation is the thing that destroys the naive
 * delta, and rotation is - to first order - a horizontal SHIFT of the image,
 * which is the one screen transform the Game Gear does in hardware. So also
 * measure the delta after allowing the best whole-column shift, which is the
 * ceiling for anything scroll-compensated. */
static unsigned long n_shift_cells_same[NBUCKET], n_shift_wk_skip[NBUCKET];
static unsigned long n_shift_cols_skip_live[NBUCKET];
static unsigned long r_shift_cells_same[8], r_shift_wk_skip[8];
static unsigned long r_shift_cols_skip_live[8];
static long sum_best_shift_err = 0, n_best_shift = 0;

/* Attribution of a changed run, pure-rotation regime especially: which of the
 * run's parameters actually moved? */
static unsigned long a_runs, a_depth_same, a_width_same, a_c0_same;
static unsigned long a_step_same, a_depth_and_width_same;
/* Split by whether the run's endpoints are REAL wall corners or FOV clips. A
 * real corner's range is invariant under pure camera rotation; a clipped
 * endpoint sits at a fixed bearing (+/-512), so rotation slides it along the
 * wall and its range changes. If the 1% depth stability under rotation is
 * clipping rather than geometry, persistent span state survives for the
 * unclipped runs and only clipped ones need rework. */
static unsigned long a_both_real, a_both_real_depth_same;
static unsigned long a_clipped, a_clipped_depth_same;

/* Row-write level, the exact temporal analogue of A25's 9.1% spatial figure:
 * of the row-writes the materializer performs, how many store a word that is
 * ALREADY there from last update? This is the ceiling for eliminating stores,
 * and it is deliberately measured the same way A25 measured overdraw so the
 * two numbers can be compared directly. */
static unsigned long n_rowwrites[NBUCKET], n_rowwrites_same[NBUCKET];
static unsigned long r_rowwrites[8], r_rowwrites_same[8];

/* Per-regime accumulators; the regime table lives next to main. */
#define NREG_MAX 8u
static unsigned long r_pairs[NREG_MAX], r_cells_same[NREG_MAX];
static unsigned long r_cols_same[NREG_MAX];
static unsigned long r_cols_live[NREG_MAX], r_cols_skip_live[NREG_MAX];
static unsigned long r_wk[NREG_MAX], r_wk_skip[NREG_MAX];
static unsigned long r_dyaw[NREG_MAX], r_dpos[NREG_MAX];
static unsigned g_reg;

static unsigned bucket_of(unsigned dyaw)
{
    if (dyaw == 0) return 0;
    if (dyaw <= 2) return 1;
    if (dyaw <= 4) return 2;
    if (dyaw <= 8) return 3;
    if (dyaw <= 16) return 4;
    return 5;
}

static void compare(const Snapshot *prev, const Snapshot *cur)
{
    unsigned c, r, k;
    int dy = (int)cur->yaw - (int)prev->yaw;
    unsigned dyaw, dpos;
    unsigned bkt;
    int dx = cur->x_q4 - prev->x_q4, dyp = cur->y_q4 - prev->y_q4;

    if (dy > 128) dy -= 256;
    if (dy < -128) dy += 256;
    dyaw = (unsigned)(dy < 0 ? -dy : dy);
    dpos = (unsigned)((dx < 0 ? -dx : dx) + (dyp < 0 ? -dyp : dyp));
    bkt = bucket_of(dyaw);

    ++n_pairs[bkt];
    sum_dyaw[bkt] += dyaw;
    sum_dpos[bkt] += dpos;
    n_wk[bkt] += cur->nwk;
    ++r_pairs[g_reg];
    r_dyaw[g_reg] += dyaw;
    r_dpos[g_reg] += dpos;
    r_wk[g_reg] += cur->nwk;

    for (k = 0; k < cur->nwk; ++k) {
        const WorkKey *w = &cur->wk[k];
        int rr;
        for (rr = w->lo; rr <= w->hi; ++rr) {
            ++n_rowwrites[bkt]; ++r_rowwrites[g_reg];
            if (w->word[rr] == prev->map[k_row_base[rr] + w->col]) {
                ++n_rowwrites_same[bkt]; ++r_rowwrites_same[g_reg];
            }
        }
    }

    for (k = 0; k < cur->nwk; ++k) {
        unsigned j, cc = cur->wk[k].col;
        for (j = 0; j < prev->col_n[cc]; ++j)
            if (wk_eq(&cur->wk[k], &prev->wk[prev->col_first[cc] + j])) {
                ++n_wk_same[bkt];
                break;
            }
    }

    for (c = 0; c < TSP_COLS; ++c) {
        int cells_same = 1, list_same, live;
        unsigned long same_here = 0;
        for (r = 0; r < TSP_ROWS; ++r)
            if (cur->map[k_row_base[r] + c] == prev->map[k_row_base[r] + c])
                ++same_here;
        cells_same = (same_here == TSP_ROWS);

        /* A column is LIVE if either update materialized anything into it.
         * Dead columns are background in both and trivially "unchanged" -
         * counting them as a temporal win would be counting the 20-column
         * screen's empty space as an optimisation. They carry no work units,
         * so they cannot inflate the work figure, but they badly inflate any
         * column percentage, which is why this split exists. */
        live = (cur->col_n[c] || prev->col_n[c]);

        list_same = (cur->col_n[c] == prev->col_n[c]);
        if (list_same)
            for (k = 0; k < cur->col_n[c]; ++k)
                if (!wk_eq(&cur->wk[cur->col_first[c] + k],
                           &prev->wk[prev->col_first[c] + k])) {
                    list_same = 0; break;
                }

        if (cells_same) { ++n_cols_same[bkt]; ++r_cols_same[g_reg]; }
        if (live) { ++n_cols_live[bkt]; ++r_cols_live[g_reg]; }
        if (list_same) {
            ++n_cols_skippable[bkt];
            n_wk_in_skippable[bkt] += cur->col_n[c];
            r_wk_skip[g_reg] += cur->col_n[c];
            if (live) { ++n_cols_skip_live[bkt]; ++r_cols_skip_live[g_reg]; }
            /* An identical ordered run list MUST produce identical cells.
             * If this fires, the work key is missing an input and every
             * number below is worthless. */
            if (!cells_same) ++invariant_violations;
        }
        n_cells_same[bkt] += same_here;
        r_cells_same[g_reg] += same_here;
    }

    /* Attribute each run's change. Matched by sid, which is the run's stable
     * world identity across updates. */
    {
        unsigned p, q;
        for (p = 0; p < cur->nrp; ++p) {
            for (q = 0; q < prev->nrp; ++q) {
                const RunParam *A = &cur->rp[p], *B = &prev->rp[q];
                int wA, wB;
                if (A->sid != B->sid) continue;
                wA = (int)A->c1 - (int)A->c0;
                wB = (int)B->c1 - (int)B->c0;
                ++a_runs;
                if (A->inv0 == B->inv0 && A->inv1 == B->inv1) ++a_depth_same;
                if (wA == wB) ++a_width_same;
                if (A->c0 == B->c0) ++a_c0_same;
                if (A->step == B->step) ++a_step_same;
                if (A->inv0 == B->inv0 && A->inv1 == B->inv1 && wA == wB)
                    ++a_depth_and_width_same;
                if (A->left_real && A->right_real
                    && B->left_real && B->right_real) {
                    ++a_both_real;
                    if (A->inv0 == B->inv0 && A->inv1 == B->inv1)
                        ++a_both_real_depth_same;
                } else {
                    ++a_clipped;
                    if (A->inv0 == B->inv0 && A->inv1 == B->inv1)
                        ++a_clipped_depth_same;
                }
                break;
            }
        }
    }

    /* Best whole-column shift. Search every shift rather than deriving one
     * from dyaw: the point is to find the CEILING a perfect scroll
     * compensation could reach, and a derived shift would fold the
     * projection's nonlinearity into the answer instead of exposing it. */
    {
        int d, best_d = 0;
        unsigned long best_cells = 0, best_wk = 0, best_cols = 0;
        for (d = -(int)TSP_COLS + 1; d < (int)TSP_COLS; ++d) {
            unsigned long cells = 0, wk = 0, cols = 0;
            for (c = 0; c < TSP_COLS; ++c) {
                int pc = (int)c - d;
                unsigned live, list_same;
                if (pc < 0 || pc >= (int)TSP_COLS) continue;
                for (r = 0; r < TSP_ROWS; ++r)
                    if (cur->map[k_row_base[r] + c]
                        == prev->map[k_row_base[r] + pc]) ++cells;
                list_same = (cur->col_n[c] == prev->col_n[pc]);
                if (list_same)
                    for (k = 0; k < cur->col_n[c]; ++k)
                        if (!wk_eq(&cur->wk[cur->col_first[c] + k],
                                   &prev->wk[prev->col_first[pc] + k])) {
                            list_same = 0; break;
                        }
                live = (cur->col_n[c] || prev->col_n[pc]);
                if (list_same) { wk += cur->col_n[c]; if (live) ++cols; }
            }
            if (cells > best_cells) {
                best_cells = cells; best_d = d;
            }
            if (wk > best_wk) best_wk = wk;
            if (cols > best_cols) best_cols = cols;
        }
        n_shift_cells_same[bkt] += best_cells;
        r_shift_cells_same[g_reg] += best_cells;
        n_shift_wk_skip[bkt] += best_wk;
        r_shift_wk_skip[g_reg] += best_wk;
        n_shift_cols_skip_live[bkt] += best_cols;
        r_shift_cols_skip_live[g_reg] += best_cols;
        /* How far is the best empirical shift from the one a linear
         * (cylindrical) projection would predict? 1 yaw unit = 16 q12, and
         * the centre of the screen runs at 8 q12 per pixel = 64 per column,
         * so a linear model predicts dyaw/4 columns. */
        {
            int predicted = -(int)((cur->yaw - prev->yaw) & 0xffu);
            if (predicted < -128) predicted += 256;
            predicted = predicted / 4;
            sum_best_shift_err += (best_d - predicted) < 0
                                ? (predicted - best_d) : (best_d - predicted);
            ++n_best_shift;
        }
    }
}

/* Motion REGIMES, each run from many spawn points and yaws.
 *
 * The first cut of this probe used eight hand-placed trajectories and averaged
 * 14.08 run-columns per update against the 29.27 A25 measured over the whole
 * walkable grid - i.e. it was sampling a corridor, not the map, and every
 * number it produced was about that corridor. Regimes are now crossed with a
 * spawn grid so the workload matches A25's. */
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

int main(int argc, char **argv)
{
    unsigned U = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 4u;
    unsigned frames = (argc > 2) ? (unsigned)strtoul(argv[2], 0, 0) : 240u;
    unsigned yaw_step = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 64u;
    int only = (argc > 4) ? (int)strtol(argv[4], 0, 0) : -1;  /* regime filter */
    unsigned t, f, i, gx, gy, yy;
    unsigned long tot_pairs = 0, spawns = 0;

    if (!U) U = 1;
    if (!yaw_step) yaw_step = 64u;
    g_tspf_appearance_mode = 0u;

    for (t = 0; t < NREG; ++t) {
        if (only >= 0 && (int)t != only) continue;
        g_reg = t;
        for (gy = 0; gy < 24u; ++gy) for (gx = 0; gx < 48u; ++gx) {
            int16_t px = (int16_t)(gx * 64 + 32), py = (int16_t)(gy * 64 + 32);
            uint16_t gi;
            if (!tsp_is_walkable_q4(px, py)) continue;
            gi = (uint16_t)(((uint16_t)gy << 5) + ((uint16_t)gy << 4) + gx);
            if (k_tspf_recipe_grid[gi] == 0xffu) continue;
            for (yy = 0; yy < 256u; yy += yaw_step) {
                TSPState st;
                Snapshot *prev = &g_a, *cur = &g_b, *sw;
                int have_prev = 0;
                tsp_reset(&st);
                st.x_q4 = px; st.y_q4 = py;
                st.yaw = (uint8_t)yy; st.manual = k_regime[t].manual;
                if (t == 0) ++spawns;
                for (f = 0; f < frames; ++f) {
                    tsp_step(&st, k_regime[t].inp[(f / 30u) % 8u]);
                    if ((f % U) != U - 1) continue;
                    snapshot(&st, cur);
                    if (have_prev) { compare(prev, cur); ++tot_pairs; }
                    have_prev = 1;
                    sw = prev; prev = cur; cur = sw;
                }
            }
        }
    }

    printf("=== TEMPORAL DELTA: what actually changes between updates ===\n");
    printf("update period U = %u frames   regimes %u   spawns %lu"
           "   yaw step %u   update pairs %lu\n",
           U, (unsigned)NREG, spawns, yaw_step, tot_pairs);
    printf("(the motion model steps every FRAME; an update every %u of them)\n\n", U);

    printf("BY MOTION REGIME\n");
    printf("%-17s %8s %7s %8s | %7s %7s %9s %9s\n",
           "regime", "pairs", "dyaw", "dpos_q4",
           "cells", "live", "SKIPPABLE", "work");
    printf("%-17s %8s %7s %8s | %7s %7s %9s %9s\n",
           "", "", "mean", "mean", "same %", "cols", "live col %", "skipped %");
    for (i = 0; i < NREG; ++i) {
        if (!r_pairs[i]) continue;
        printf("%-17s %8lu %7.2f %8.1f | %7.1f %7.2f %9.1f %9.1f\n",
               k_regime[i].name, r_pairs[i],
               (double)r_dyaw[i] / r_pairs[i],
               (double)r_dpos[i] / r_pairs[i],
               100.0 * r_cells_same[i] / (r_pairs[i] * (double)TSP_MAP_CELLS),
               (double)r_cols_live[i] / r_pairs[i],
               r_cols_live[i] ? 100.0 * r_cols_skip_live[i] / r_cols_live[i] : 0.0,
               r_wk[i] ? 100.0 * r_wk_skip[i] / r_wk[i] : 0.0);
    }

    printf("\nROW-WRITE LEVEL (compare directly with A25's spatial 9.1%%)\n");
    printf("%-17s %10s %12s %12s\n",
           "regime", "row-wr/upd", "already there", "%");
    for (i = 0; i < NREG; ++i) {
        if (!r_pairs[i]) continue;
        printf("%-17s %10.2f %12.2f %11.1f%%\n",
               k_regime[i].name,
               (double)r_rowwrites[i] / r_pairs[i],
               (double)r_rowwrites_same[i] / r_pairs[i],
               r_rowwrites[i] ? 100.0 * r_rowwrites_same[i] / r_rowwrites[i] : 0.0);
    }

    printf("\nSAME, BUT ALLOWING A BEST WHOLE-COLUMN SHIFT\n");
    printf("(the ceiling if a horizontal scroll absorbed the rotation)\n");
    printf("%-17s %8s | %7s %9s %9s\n",
           "regime", "pairs", "cells", "SKIPPABLE", "work");
    printf("%-17s %8s | %7s %9s %9s\n",
           "", "", "same %", "live col %", "skipped %");
    for (i = 0; i < NREG; ++i) {
        if (!r_pairs[i]) continue;
        printf("%-17s %8lu | %7.1f %9.1f %9.1f\n",
               k_regime[i].name, r_pairs[i],
               100.0 * r_shift_cells_same[i] / (r_pairs[i] * (double)TSP_MAP_CELLS),
               r_cols_live[i] ? 100.0 * r_shift_cols_skip_live[i] / r_cols_live[i] : 0.0,
               r_wk[i] ? 100.0 * r_shift_wk_skip[i] / r_wk[i] : 0.0);
    }

    printf("\nBY |dyaw| PER UPDATE\n");
    printf("%-12s %8s %7s %8s | %7s %7s %9s %9s\n",
           "bucket", "pairs", "dyaw", "dpos_q4",
           "cells", "live", "SKIPPABLE", "work");
    {
        static const char *bn[NBUCKET] = { "0 (no turn)", "1-2", "3-4", "5-8",
                                           "9-16", ">16" };
        unsigned long P = 0, CS = 0, KS = 0, WS = 0, W = 0, CL = 0, DY = 0, DP = 0;
        for (i = 0; i < NBUCKET; ++i) {
            if (!n_pairs[i]) continue;
            printf("%-12s %8lu %7.2f %8.1f | %7.1f %7.2f %9.1f %9.1f\n",
                   bn[i], n_pairs[i],
                   (double)sum_dyaw[i] / n_pairs[i],
                   (double)sum_dpos[i] / n_pairs[i],
                   100.0 * n_cells_same[i] / (n_pairs[i] * (double)TSP_MAP_CELLS),
                   (double)n_cols_live[i] / n_pairs[i],
                   n_cols_live[i] ? 100.0 * n_cols_skip_live[i] / n_cols_live[i] : 0.0,
                   n_wk[i] ? 100.0 * n_wk_in_skippable[i] / n_wk[i] : 0.0);
            P += n_pairs[i]; CS += n_cells_same[i]; CL += n_cols_live[i];
            KS += n_cols_skip_live[i]; WS += n_wk_in_skippable[i];
            W += n_wk[i]; DY += sum_dyaw[i]; DP += sum_dpos[i];
        }
        printf("%-12s %8lu %7.2f %8.1f | %7.1f %7.2f %9.1f %9.1f\n",
               "ALL", P, (double)DY / P, (double)DP / P,
               100.0 * CS / (P * (double)TSP_MAP_CELLS),
               (double)CL / P,
               CL ? 100.0 * KS / CL : 0.0,
               W ? 100.0 * WS / W : 0.0);

        printf("\nwork units (run-columns) per update       %.2f"
               "   (A25 measured 29.27 over the same grid)\n", (double)W / P);
        printf("same (sid,col) key present last update    %.1f%%"
               "   <- LOOSE bound: ignores ordering, not achievable\n",
               W ? 100.0 * (n_wk_same[0]+n_wk_same[1]+n_wk_same[2]+n_wk_same[3]
                           +n_wk_same[4]+n_wk_same[5]) / W : 0.0);
        printf("\nCEILING on materialize (194,761 T/update): %.1f%% skipped"
               " -> %.0f T\n",
               W ? 100.0 * WS / W : 0.0,
               194761.0 * (1.0 - (W ? (double)WS / W : 0.0)));
        printf("EXCLUDING the stand-still regime:          %.1f%% skipped\n",
               (W - r_wk[0]) ? 100.0 * (double)(WS - r_wk_skip[0])
                             / (double)(W - r_wk[0]) : 0.0);
        {
            unsigned long SW = 0, SC = 0, SL = 0;
            for (i = 0; i < NBUCKET; ++i) {
                SW += n_shift_wk_skip[i]; SC += n_shift_cells_same[i];
                SL += n_shift_cols_skip_live[i];
            }
            printf("\nWITH A BEST WHOLE-COLUMN SHIFT (scroll-compensated):\n");
            printf("  cells unchanged                         %.1f%%"
                   "   (unshifted %.1f%%)\n",
                   100.0 * SC / (P * (double)TSP_MAP_CELLS),
                   100.0 * CS / (P * (double)TSP_MAP_CELLS));
            printf("  live columns skippable                  %.1f%%"
                   "   (unshifted %.1f%%)\n",
                   CL ? 100.0 * SL / CL : 0.0, CL ? 100.0 * KS / CL : 0.0);
            printf("  work skipped                            %.1f%%"
                   "   (unshifted %.1f%%)\n",
                   W ? 100.0 * SW / W : 0.0, W ? 100.0 * WS / W : 0.0);
            printf("  best shift vs linear-projection predict %.2f columns"
                   " mean |error|\n",
                   n_best_shift ? (double)sum_best_shift_err / n_best_shift : 0.0);
            printf("  CEILING on materialize                  %.0f T"
                   " (from 194,761)\n",
                   194761.0 * (1.0 - (W ? (double)SW / W : 0.0)));
        }
        {
            unsigned long RW = 0, RS = 0;
            for (i = 0; i < NBUCKET; ++i) {
                RW += n_rowwrites[i]; RS += n_rowwrites_same[i];
            }
            printf("\nROW-WRITE LEVEL, ALL REGIMES:"
                   " %.2f row-writes/update, %.1f%% already carry the right"
                   " word\n", (double)RW / P, RW ? 100.0 * RS / RW : 0.0);
            printf("  (A25's SPATIAL equivalent, overdraw within one frame,"
                   " was 9.1%%)\n");
            printf("  This bounds STORE elimination only. It is far above the"
                   " work figure above\n"
                   "  because a column's TILE survives geometry changes its"
                   " derivation inputs do not.\n");
        }
    }
    if (a_runs) {
        printf("\nWHY A WORK KEY CHANGES - run-parameter attribution"
               " (%lu matched runs)\n", a_runs);
        printf("  projected depth pair (inv0,inv1) unchanged   %.1f%%\n",
               100.0 * a_depth_same / a_runs);
        printf("  run WIDTH in columns unchanged               %.1f%%\n",
               100.0 * a_width_same / a_runs);
        printf("  run START column unchanged                   %.1f%%\n",
               100.0 * a_c0_same / a_runs);
        printf("  per-column depth STEP unchanged              %.1f%%\n",
               100.0 * a_step_same / a_runs);
        printf("  depth AND width both unchanged               %.1f%%\n",
               100.0 * a_depth_and_width_same / a_runs);
        printf("  --- split by endpoint kind ---\n");
        printf("  both endpoints REAL corners   %6.1f%% of runs,"
               "  depth unchanged %.1f%%\n",
               100.0 * a_both_real / a_runs,
               a_both_real ? 100.0 * a_both_real_depth_same / a_both_real : 0.0);
        printf("  at least one FOV-CLIPPED      %6.1f%% of runs,"
               "  depth unchanged %.1f%%\n",
               100.0 * a_clipped / a_runs,
               a_clipped ? 100.0 * a_clipped_depth_same / a_clipped : 0.0);
    }

    printf("\nINVARIANT (identical run list => identical cells): %s"
           "  (%lu violations)\n",
           invariant_violations ? "*** VIOLATED ***" : "holds",
           invariant_violations);
    return invariant_violations ? 1 : 0;
}
