#ifndef RACE_KERNELS_H
#define RACE_KERNELS_H
/* The three kernels, shared verbatim between the Game Gear ROM and the host
 * checker, so that what is debugged on the host is exactly what is timed on the
 * Z80. Keeping them in the ROM only meant every correctness question cost a full
 * build-and-emulate cycle. */
#include <stdint.h>
#include "race_data.h"

#define MAXMV 40
#define CHUNK 6

/* sampled per instruction by the profiler to attribute master cycles */

/* ---- phase 1: arithmetic DDA ------------------------------------------- */
static uint8_t dda_span(int16_t iq, int16_t step, uint8_t ncols, uint8_t *out)
{
    int16_t a = (int16_t)(iq + 32);
    uint8_t n = 0, c;
    int8_t r0, r1, r2;
    /* rows for the first two column edges */
    r0 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    a = (int16_t)(a + step);
    r1 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    for (c = 0; c < ncols; ++c) {
        int8_t lo = r0 < r1 ? r0 : r1, hi = r0 < r1 ? r1 : r0, nd, lo1;
        for (nd = (int8_t)(hi - lo); nd > 0; --nd) out[n++] = 0;
        if (c + 1u == ncols) { out[n++] = 6; break; }
        a = (int16_t)(a + step);
        r2 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
        lo1 = r1 < r2 ? r1 : r2;
        out[n++] = (uint8_t)(1 - (int8_t)(lo1 - hi));
        r0 = r1; r1 = r2;
    }
    return n;
}

/* ---- phases 2 and 3: the band transducer -------------------------------- */
/* Per run: the eight phases at which a six-column behaviour can change. */
static uint16_t g_edge[8];
static uint8_t g_prog[8][CHUNK];   /* memoised six-column programs, packed */
static uint8_t g_prog_have;        /* one valid bit per band */

static void band_setup(int16_t step)
{
    uint8_t c;
    int16_t acc = 0;
    g_prog_have = 0;
    for (c = 0; c < 8; ++c) { g_edge[c] = (uint16_t)(acc & 1023); acc = (int16_t)(acc - step); }
}
/* Band index: the number of edges at or below this phase, minus one.
 *
 * The earlier analysis called this "a rank among eight computed phases", which
 * was a hand-wave and not a valid index: counting edges within half a turn
 * behind the phase puts two DIFFERENT bands on the same number, so a memoised
 * program gets replayed for a chunk it does not describe. That is exactly what
 * the race caught on the second chunk of every multi-chunk span.
 *
 * Counting edges at or below the phase is a correct interval index, and there is
 * no wrap case to handle because the c = 0 edge is always at phase 0: -0*step is
 * 0 for every step. Coincident edges (step 0 collapses all eight onto 0) simply
 * land every phase in the same band, which is right. */
static uint8_t band_of(uint16_t phase)
{
    uint8_t c, r = 0;
    for (c = 0; c < 8; ++c) if (g_edge[c] <= phase) ++r;
    return (uint8_t)(r - 1u);
}
/* the six-column program for this chunk, packed one byte per column as
   ndown*8 + (-jump); computed once per band and then replayed */
static void band_build(int16_t a0, int16_t step, uint8_t *p)
{
    int16_t a = a0;
    uint8_t c;
    int8_t r0, r1, r2;
    r0 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    a = (int16_t)(a + step);
    r1 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
    for (c = 0; c < CHUNK; ++c) {
        int8_t lo = r0 < r1 ? r0 : r1, hi = r0 < r1 ? r1 : r0, lo1;
        a = (int16_t)(a + step);
        r2 = (int8_t)((int16_t)(71 - (a >> 7)) >> 3);
        lo1 = r1 < r2 ? r1 : r2;
        p[c] = (uint8_t)(((uint8_t)(hi - lo) << 3) | (uint8_t)(hi - lo1));
        r0 = r1; r1 = r2;
    }
}
static uint8_t band_span(int16_t iq, int16_t step, uint8_t ncols, uint8_t *out)
{
    uint8_t n = 0, left = ncols, c;
    int16_t a = (int16_t)(iq + 32);
    while (left) {
        uint8_t want = left > CHUNK ? CHUNK : left, b, *p;
        uint8_t bit;
        b = band_of((uint16_t)(a & 1023));
        bit = (uint8_t)(1u << (b & 7u));
        p = g_prog[b & 7u];
        if (!(g_prog_have & bit)) { band_build(a, step, p); g_prog_have |= bit; }
        for (c = 0; c < want; ++c) {
            uint8_t v = p[c], nd = (uint8_t)(v >> 3);
            while (nd--) out[n++] = 0;
            if (left - c == 1u) { out[n++] = 6; break; }
            out[n++] = (uint8_t)(1u + (v & 7u));
        }
        if (left <= CHUNK) break;
        left = (uint8_t)(left - CHUNK);
        a = (int16_t)(a + (int16_t)(CHUNK * step));
    }
    return n;
}

/* ---- phase 4: packed replay -------------------------------------------- */
static uint8_t pack_span(const uint8_t *src, uint8_t mlen, uint8_t *out)
{
    uint8_t i;
    for (i = 0; i < mlen; ++i) out[i] = src[i];
    return mlen;
}

#endif
