/*
 * Dump the SHIPPED C's own column-solve intermediates on real camera poses.
 *
 * The Z80 column-solve kernel needs an oracle, and a Python re-derivation of
 * the C would be a second guess, not an oracle -- the same trap the wrap-
 * threshold bug came from (docs/TODO_DEFERRED.md A7). So this includes
 * tilesector_polar_renderer.c directly, which makes its `static` internals
 * callable without modifying one line of shipped renderer source, and dumps
 * what the real code actually computes.
 *
 * SELF-CHECKING DUMP
 * ------------------
 * project_key() does not hand back `lo`/`hi`; it consumes them internally.
 * They are re-derived here to feed inv_at_invd -- which would reintroduce
 * exactly the re-derivation risk this file exists to avoid, except that the
 * re-derivation is then CHECKED against the oracle: if the locally computed
 * lo/hi were wrong, inv_at_invd(...lo) could not reproduce the inv0/inv1
 * that the real project_key just returned. Any mismatch aborts the dump
 * rather than emitting a plausible-looking wrong row.
 *
 * Emits one row per visible span:
 *   x_q4 y_q4 yaw sid lo hi x0 x1 invd inv0 inv1 c0 c1 n iq step
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tilesector_polar_renderer.c"

#define GRID_W 48u
#define GRID_H 24u
#define CELL_Q4 64

int main(int argc, char **argv) {
    unsigned yaw_step = (argc > 1) ? (unsigned)strtoul(argv[1], 0, 0) : 8u;
    const char *out_path = (argc > 2) ? argv[2] : "-";
    unsigned stride = (argc > 3) ? (unsigned)strtoul(argv[3], 0, 0) : 1u;
    FILE *out = (strcmp(out_path, "-") == 0) ? stdout : fopen(out_path, "w");
    TSPState s;
    PolarRun r;
    uint32_t gx, gy, yaw, k;
    unsigned long emitted = 0, seen = 0, checked = 0;

    if (!out) { fprintf(stderr, "cannot open %s\n", out_path); return 1; }
    tsp_polar_renderer_reset();

    for (gy = 0; gy < GRID_H; ++gy) {
        for (gx = 0; gx < GRID_W; ++gx) {
            int16_t px = (int16_t)(gx * CELL_Q4 + CELL_Q4 / 2);
            int16_t py = (int16_t)(gy * CELL_Q4 + CELL_Q4 / 2);
            if (!tsp_is_walkable_q4(px, py)) continue;
            for (yaw = 0; yaw < 256u; yaw += yaw_step) {
                memset(&s, 0, sizeof(s));
                s.x_q4 = px; s.y_q4 = py; s.yaw = (uint8_t)yaw;
                for (k = 0; k < TSPF_KEY_COUNT; ++k) {
                    uint16_t w, a0, a1, len, yawq;
                    int16_t st, en, lo, hi, dq4;
                    uint8_t sid, v0, v1, invd, i0, i1, c0, c1, n;
                    int16_t iq, step;

                    memset(&r, 0, sizeof(r));
                    if (!project_key((uint8_t)k, &s, &r)) continue;
                    ++seen;

                    /* Re-derive lo/hi exactly as project_key does, then prove
                     * the re-derivation right against the oracle's own
                     * inv0/inv1 before trusting it into the dump. */
                    w = k_tspf_keys[k];
                    sid = (uint8_t)(w & 31u);
                    v0 = (uint8_t)((w >> 5) & 15u);
                    v1 = (uint8_t)((w >> 9) & 15u);
                    a0 = bearing_vertex_q12(v0, &s);
                    a1 = bearing_vertex_q12(v1, &s);
                    len = (uint16_t)((a1 - a0) & 4095u);
                    yawq = (uint16_t)s.yaw << 4;
                    st = signed_q12((uint16_t)(a0 - yawq));
                    en = (int16_t)(st + (int16_t)len);
                    while (en < -512) { st = (int16_t)(st + 4096); en = (int16_t)(en + 4096); }
                    while (st > 512) { st = (int16_t)(st - 4096); en = (int16_t)(en - 4096); }
                    lo = st < -512 ? -512 : st;
                    hi = en > 512 ? 512 : en;

                    dq4 = wall_d_q4(sid, k_tspf_seg_anchor[sid], &s);
                    invd = inv_for_dq4(dq4);
                    i0 = inv_at_invd(sid, invd, (uint16_t)(yawq + lo) & 4095u, lo);
                    i1 = inv_at_invd(sid, invd, (uint16_t)(yawq + hi) & 4095u, hi);

                    if (i0 != r.inv0 || i1 != r.inv1) {
                        fprintf(stderr,
                                "ORACLE MISMATCH at (%d,%d,yaw=%u,key=%u): "
                                "re-derived inv0/inv1 = %u/%u, project_key = %u/%u\n",
                                px, py, yaw, k, i0, i1, r.inv0, r.inv1);
                        return 2;
                    }
                    ++checked;

                    c0 = (uint8_t)(r.x0 >> 3); c1 = (uint8_t)(r.x1 >> 3);
                    if (c0 >= TSP_COLS) c0 = (uint8_t)(TSP_COLS - 1u);
                    if (c1 >= TSP_COLS) c1 = (uint8_t)(TSP_COLS - 1u);
                    if (c1 < c0) continue;
                    n = (uint8_t)(c1 - c0 + 1u);
                    iq = (int16_t)((int16_t)r.inv0 << 6);
                    step = (int16_t)(((int16_t)r.inv1 - (int16_t)r.inv0)
                                     * (int16_t)k_col_recip_q8[n]);
                    step = shr_signed(step, 2);

                    if ((seen - 1) % stride) continue;
                    fprintf(out, "%d %d %u %u %d %d %u %u %u %u %u %u %u %u %d %d\n",
                            px, py, yaw, sid, lo, hi, r.x0, r.x1,
                            invd, r.inv0, r.inv1, c0, c1, n, iq, step);
                    ++emitted;
                }
            }
        }
    }
    if (out != stdout) fclose(out);
    fprintf(stderr, "visible spans %lu, oracle-checked %lu, rows emitted %lu"
                    " (stride %u)\n", seen, checked, emitted, stride);
    return 0;
}
