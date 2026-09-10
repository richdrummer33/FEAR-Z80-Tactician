/*
 * Shared retained-span state model for the temporal probes.
 *
 * A29 established that the renderer's per-column output is a pure function of
 * six values - (tl, tr, bl, br, shade, border) - through draw_edge(top),
 * draw_edge(bottom), draw_full(interior) in that order, interior last so it
 * wins on overlap. Nothing else about the projection can reach a cell.
 *
 * This header is the ONE definition of that model. temporal_boundary_probe.c
 * (exact path, A29/A30/A31) and temporal_error_probe.c (bounded-error path)
 * both include it. Duplicating it in each probe is exactly where a silent
 * drift between the exact and approximate paths would hide, and the whole
 * point of the second probe is that its E=0 case must reduce to the first.
 *
 * Include AFTER tilesector_polar_renderer.c - it uses row_floor, edge_entry,
 * clamp_s8 and TSP_TILE_FULL from there.
 */
#ifndef TEMPORAL_SPAN_STATE_H
#define TEMPORAL_SPAN_STATE_H

#ifndef MAXSPAN
#define MAXSPAN TSPF_MAX_ACTIVE
#endif

typedef struct {
    uint8_t present;
    int16_t tl, tr, bl, br;
    uint8_t shade, border;
} ColState;

typedef struct {
    uint8_t keyid;                 /* persistent span identity across poses */
    uint8_t rank;                  /* far->near draw index */
    uint8_t x0, x1;                /* screen PIXEL extent, for sub-cell motion */
    uint8_t c0, c1;
    int16_t iq, step;              /* run params, for the sequence oracle */
    uint8_t profile, left_real, right_real, shade_run;
    ColState col[TSP_COLS];
} SpanState;

typedef struct {
    uint16_t map[TSP_MAP_CELLS];
    SpanState sp[MAXSPAN];
    uint8_t nsp;
    int16_t x_q4, y_q4;
    uint8_t yaw;
} Pose;

/* ---- exact re-derivations of the renderer's own row ranges ---- */

static void top_rows(const ColState *s, int *r0, int *r1)
{
    int a = row_floor(s->tl < s->tr ? s->tl : s->tr);
    int b = row_floor(s->tl > s->tr ? s->tl : s->tr);
    if (a < 0) a = 0;
    if (b >= (int)TSP_ROWS) b = (int)TSP_ROWS - 1;
    *r0 = a; *r1 = b;
}
static void bot_rows(const ColState *s, int *r0, int *r1)
{
    int a = row_floor(s->bl < s->br ? s->bl : s->br);
    int b = row_floor(s->bl > s->br ? s->bl : s->br);
    if (a < 0) a = 0;
    if (b >= (int)TSP_ROWS) b = (int)TSP_ROWS - 1;
    *r0 = a; *r1 = b;
}
static void int_rows(const ColState *s, int *r0, int *r1)
{
    int a = row_floor(s->tl > s->tr ? s->tl : s->tr) + 1;
    int b = row_floor(s->bl < s->br ? s->bl : s->br) - 1;
    if (a < 0) a = 0;
    if (b >= (int)TSP_ROWS) b = (int)TSP_ROWS - 1;
    *r0 = a; *r1 = b;
}

/* This span's contribution to one cell, or 0 if it writes nothing there.
 * Mirrors draw_run's within-column order: top, then bottom, then interior. */
static int contrib(const ColState *s, int row, uint16_t *w)
{
    int a, b;
    if (!s->present) return 0;
    int_rows(s, &a, &b);
    if (a <= b && row >= a && row <= b) {
        *w = TSP_TILE_FULL(s->shade, TSP_CAP_NONE, s->border);
        return 1;
    }
    bot_rows(s, &a, &b);
    if (row >= a && row <= b) {
        *w = edge_entry(s->shade, (int16_t)(s->bl - (row << 3)),
                        clamp_s8((int16_t)(s->br - s->bl), -7, 7), 1u);
        return 1;
    }
    top_rows(s, &a, &b);
    if (row >= a && row <= b) {
        *w = edge_entry(s->shade, (int16_t)(s->tl - (row << 3)),
                        clamp_s8((int16_t)(s->tr - s->tl), -7, 7), 0u);
        return 1;
    }
    return 0;
}

static int col_same(const ColState *a, const ColState *b)
{
    if (a->present != b->present) return 0;
    if (!a->present) return 1;
    return a->tl == b->tl && a->tr == b->tr && a->bl == b->bl
        && a->br == b->br && a->shade == b->shade && a->border == b->border;
}

#endif /* TEMPORAL_SPAN_STATE_H */
