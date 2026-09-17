#ifndef RUNG1_HELPER_VECTORS_H
#define RUNG1_HELPER_VECTORS_H
/* Canonical test vectors for the small fixed-point helpers that exist in more
 * than one copy in this tree.
 *
 * The ratio helper had been duplicated verbatim into the lattice floor-light
 * runtime, so when the wrap-to-zero bug was fixed in the renderer a corrected
 * bug survived in the copy. Sharing one implementation is awkward here -- the
 * copies live in separately banked translation units emitted by different
 * generators -- so the next best thing is that every copy is held to the same
 * vectors, and that no copy is allowed to drift in shape.
 *
 * Expected values are derived from the IDEAL, round(n*256/d), not from the
 * implementation. Recording whatever the code currently prints would make the
 * suite circular and unable to catch the next regression. Where the helper's
 * reciprocal table legitimately costs an LSB, the vector carries an explicit
 * tolerance of 1 and says so, rather than the expectation being quietly bent to
 * match. Everything that matters carries a tolerance of ZERO:
 *
 *   unity          the true Q8 value is 256, must saturate to 255, never wrap
 *   zero           numerator zero, and the guarded zero denominator
 *   binary         exact binary fractions, where rounding cannot hide an error
 *   near-unity     must come close WITHOUT saturating
 *
 * {n, d, expected, tolerance}
 */
static const unsigned short RATIO_Q8[][4] = {
    /* unity: the regression that motivated all of this. Tolerance 0. */
    {1,1,255,0}, {2,2,255,0}, {7,7,255,0}, {63,63,255,0}, {64,64,255,0},
    {127,127,255,0}, {128,128,255,0}, {200,200,255,0}, {254,254,255,0}, {255,255,255,0},
    /* zero numerator, and the guarded zero denominator */
    {0,1,0,0}, {0,255,0,0}, {0,0,0,0}, {5,0,0,0},
    /* exact binary fractions */
    {1,2,128,0}, {1,4,64,0}, {3,4,192,0}, {1,8,32,0}, {7,8,224,0},
    {1,16,16,0}, {15,16,240,0},
    /* near unity from below: close, but must not saturate */
    {254,255,255,0}, {127,128,254,0}, {63,64,252,0},
    /* awkward denominators. 100/101 is 253.47 ideally; the reciprocal table
     * rounds 65536/101 up to 649, which carries it to 254. One LSB, and real. */
    {1,3,85,0}, {2,3,171,0}, {1,7,37,0}, {6,7,219,0}, {17,19,229,0},
    {100,101,253,1},
};
#define RATIO_Q8_N ((int)(sizeof(RATIO_Q8)/sizeof(RATIO_Q8[0])))
#endif
