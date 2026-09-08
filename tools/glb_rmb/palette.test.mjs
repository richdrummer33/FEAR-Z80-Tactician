/*
 * Guard tests for the palette solver.
 *
 * Run: node tools/glb_rmb/palette.test.mjs
 *
 * These are properties, not golden values. A golden palette would break every
 * time the ramp curve is retuned and would say nothing about whether the
 * result is correct; what actually has to hold is that the ramp is monotone,
 * that it stays in gamut, that the mono/polychrome decision keys on material
 * and not on baked lighting, and that the interleave is an improvement rather
 * than a rationalisation.
 */
import assert from 'node:assert/strict';
import {
  srgb8ToOklab, oklabToLinear, linearToOklab, ggToOklab, ggQuantize,
  gamutMapOklab, synthesizeRamp, quantizeRamp, solveRamp, bestSingle, bestPair,
  interleavedOklab, materialResidual, chromaGridStep, fitRampChroma,
  kmeansOklab, oklabDistance, RAMP_MIN_STEP_L,
  chromaticity, chromaConfidence, clusterFamilies, transferFamilyToShell
} from './palette.mjs';

let failures = 0;
function test(name, fn) {
  try { fn(); console.log(`  ok  ${name}`); }
  catch (e) { failures++; console.log(`FAIL  ${name}\n      ${e.message}`); }
}

const TERRACOTTA = srgb8ToOklab(170, 68, 34);
const SLATE = srgb8ToOklab(96, 116, 150);
const BAND = [0.355, 0.790];

test('Oklab round-trips through linear sRGB', () => {
  for (const c of [[0.1, 0.2, 0.3], [0.9, 0.5, 0.05], [0.5, 0.5, 0.5]]) {
    const lab = linearToOklab(...c);
    const back = oklabToLinear(...lab);
    for (let i = 0; i < 3; ++i) assert.ok(Math.abs(back[i] - c[i]) < 1e-6);
  }
});

test('gamut mapping keeps hue and lightness, only sheds chroma', () => {
  /* A vivid red at high lightness is outside the display gamut. */
  const L = 0.85, a = 0.22, b = 0.12;
  const m = gamutMapOklab(L, a, b);
  assert.equal(m[0], L, 'lightness must not move');
  const h0 = Math.atan2(b, a), h1 = Math.atan2(m[2], m[1]);
  assert.ok(Math.abs(h0 - h1) < 1e-9, 'hue angle must not rotate');
  assert.ok(Math.hypot(m[1], m[2]) < Math.hypot(a, b), 'chroma must have fallen');
  assert.ok(oklabToLinear(...m).every(v => v <= 1 + 1e-3 && v >= -1e-3));
});

test('a synthesized ramp is monotone in lightness after quantization', () => {
  for (const lab of [TERRACOTTA, SLATE, srgb8ToOklab(30, 140, 60)]) {
    for (const stops of [4, 5, 6]) {
      const gg = quantizeRamp(synthesizeRamp(lab, stops, null, BAND));
      for (let i = 1; i < gg.length; ++i)
        assert.ok(ggToOklab(gg[i])[0] > ggToOklab(gg[i - 1])[0],
                  `stop ${i} did not get brighter (${JSON.stringify(gg)})`);
    }
  }
});

test('every ramp stop is a legal hardware colour', () => {
  const gg = quantizeRamp(synthesizeRamp(TERRACOTTA, 5, null, BAND));
  for (const c of gg)
    for (const v of c) assert.ok(Number.isInteger(v) && v >= 0 && v <= 15);
});

test('chroma tracks lightness rather than being held constant', () => {
  /* The dark end must be less chromatic than the middle, or shadows come out
   * looking like coloured plastic instead of like a surface in shadow. */
  const ramp = synthesizeRamp(TERRACOTTA, 5, null, BAND);
  const C = ramp.map(s => Math.hypot(s[1], s[2]));
  assert.ok(C[0] < C[2], `dark stop chroma ${C[0]} should be under mid ${C[2]}`);
});

test('baked lighting on one material is NOT reported as several materials', () => {
  /* Four samples of one hue at four lightnesses, chroma proportional to
   * lightness -- exactly what an image-to-3D base texture looks like. */
  const hue = [0.242, 0.176];
  const centers = [0.18, 0.31, 0.43, 0.57].map(L => [L, hue[0] * L, hue[1] * L]);
  const fit = materialResidual(centers, [0.4, 0.22, 0.3, 0.08]);
  assert.ok(fit.residual < chromaGridStep(),
            `residual ${fit.residual} should be under one grid step`);
});

test('genuinely different materials ARE reported as different', () => {
  /* Grey stone base plus a terracotta figure: same lightnesses, different
   * chromaticity. This is the case the test above must not swallow. */
  const centers = [[0.35, 0.004, 0.002], [0.35, 0.085, 0.058],
                   [0.55, 0.006, 0.003], [0.55, 0.133, 0.091]];
  const fit = materialResidual(centers, [0.3, 0.3, 0.2, 0.2]);
  assert.ok(fit.residual > chromaGridStep(),
            `residual ${fit.residual} should exceed one grid step`);
});

test('a hue that would collapse the ramp gets its chroma cut', () => {
  /* Pure saturated blue is the worst case: its lightness ceiling is very low,
   * so a wide band at full chroma cannot keep its stops apart. */
  const blue = srgb8ToOklab(20, 30, 230);
  const fit = fitRampChroma(blue, 5, [0.30, 0.85]);
  assert.ok(fit.worstGapL >= RAMP_MIN_STEP_L - 1e-6,
            `worst gap ${fit.worstGapL} is under the floor even after fitting`);
});

test('interleaving is at least as accurate as the best single colour', () => {
  const ramp = synthesizeRamp(TERRACOTTA, 5, null, BAND);
  for (const t of ramp) {
    const one = bestSingle(t), two = bestPair(t);
    assert.ok(two.err <= one.err + 1e-9,
              `pair ${two.err} worse than single ${one.err}`);
  }
});

test('interleave pairs stay under the flicker luminance limit', () => {
  for (const lab of [TERRACOTTA, SLATE]) {
    for (const s of solveRamp(synthesizeRamp(lab, 5, null, BAND))) {
      assert.ok(s.interleave.safe,
                `stop ${s.shade} luma split ${s.interleave.lumaSplit} is unsafe`);
    }
  }
});

test('interleaved colour is the LINEAR-light average, not the index average', () => {
  const a = [2, 2, 2], b = [12, 12, 12];
  const got = interleavedOklab(a, b);
  const naive = ggToOklab([7, 7, 7]);
  assert.ok(got[0] > naive[0] + 0.01,
            'averaging indices instead of light would come out too dark');
});

test('k-means is deterministic and area weighted', () => {
  const lab = new Float64Array([0.2,0.10,0.05, 0.21,0.11,0.05, 0.7,-0.02,-0.09]);
  const w = [1, 1, 8];
  const a = kmeansOklab(lab, w, 2), b = kmeansOklab(lab, w, 2);
  assert.deepEqual(Array.from(a.assign), Array.from(b.assign));
  assert.equal(new Set(a.assign).size, 2);
});

test('quantizing then reading back a hardware colour is exact', () => {
  for (const c of [[0,0,0],[15,15,15],[4,9,2]]) {
    const lab = ggToOklab(c);
    assert.deepEqual(ggQuantize(oklabToLinear(...lab)), c);
  }
});

test('the ramp separates stops without drifting off its own hue', () => {
  const ramp = synthesizeRamp(TERRACOTTA, 5, null, BAND);
  const gg = quantizeRamp(ramp);
  for (let i = 0; i < gg.length; ++i) {
    const d = oklabDistance(ggToOklab(gg[i]), ramp[i]);
    assert.ok(d < 0.12, `stop ${i} moved ${d} from its target while separating`);
  }
});

test('chromaticity divides lightness out', () => {
  /* The same material lit two ways: chroma scales with lightness, so its
     chromaticity must not move. This is the property the family clustering
     depends on, and getting it wrong is what made a red-and-green asset
     measure as one muddy hue. */
  const lit = [0.60, 0.60 * 0.24, 0.60 * 0.17];
  const shadowed = [0.18, 0.18 * 0.24, 0.18 * 0.17];
  const a = chromaticity(lit), b = chromaticity(shadowed);
  assert.ok(Math.hypot(a[0] - b[0], a[1] - b[1]) < 1e-9);
});

test('near-black samples are not trusted to define a hue', () => {
  const bright = chromaConfidence([0.60, 0.14, 0.10]);
  const dark = chromaConfidence([0.02, 0.005, 0.003]);
  assert.ok(bright > 0.9, `bright confidence ${bright}`);
  assert.ok(dark < 0.2, `dark confidence ${dark}`);
  /* A well-lit but achromatic sample also tells you nothing about hue. */
  assert.ok(chromaConfidence([0.60, 0.001, 0.001]) < 0.1);
});

test('two materials interleaved at every lightness are found as two', () => {
  /* Red petals and green leaves, each appearing across the whole lightness
     range, which is what a floral texture actually looks like. Clustering
     Oklab directly splits these by lightness and reports the hues as one;
     clustering chromaticity must not. */
  const labs = [], areas = [];
  const mats = [[0.24, 0.17], [-0.13, 0.14]];
  for (const m of mats)
    for (const L of [0.12, 0.22, 0.35, 0.50, 0.68]) {
      labs.push(L, m[0] * L, m[1] * L);
      areas.push(1);
    }
  const km = clusterFamilies(Float64Array.from(labs), areas, 2);
  /* Every sample of one material must land in one family. */
  const first = new Set(km.assign.slice(0, 5));
  const second = new Set(km.assign.slice(5));
  assert.equal(first.size, 1, `material 0 split across ${first.size} families`);
  assert.equal(second.size, 1, `material 1 split across ${second.size} families`);
  assert.notEqual([...first][0], [...second][0], 'both materials in one family');
});

test('one material at many lightnesses is not split into several', () => {
  const labs = [], areas = [];
  for (const L of [0.10, 0.18, 0.30, 0.44, 0.60, 0.75]) {
    labs.push(L, 0.24 * L, 0.17 * L);
    areas.push(1);
  }
  const km = clusterFamilies(Float64Array.from(labs), areas, 3);
  const probeL = 0.55;
  const fit = materialResidual(
    km.families.map(f => [probeL, f.chromaticity[0] * probeL, f.chromaticity[1] * probeL]),
    km.families.map(f => f.area));
  assert.ok(fit.residual < chromaGridStep(),
            `residual ${fit.residual} should say "one material"`);
});

test('family transfer takes the majority, never an average', () => {
  /* Three source vertices at the same spot: two family 2, one family 0.
     An averaging transfer would answer 1 -- a family that is not there. */
  const src = new Float64Array([0,0,0, 0.01,0,0, 0.02,0,0]);
  const fam = Int32Array.from([2, 2, 0]);
  const w = new Float64Array([1, 1, 1]);
  const dst = new Float64Array([0.01, 0, 0]);
  const out = transferFamilyToShell(src, fam, w, 3, dst, 1, 0.5, 3);
  assert.equal(out[0], 2, `majority vote returned ${out[0]}`);
});

test('family transfer falls back to the nearest source when nothing is in range', () => {
  const src = new Float64Array([5, 5, 5]);
  const out = transferFamilyToShell(src, Int32Array.from([1]),
                                    new Float64Array([1]), 1,
                                    new Float64Array([0, 0, 0]), 1, 0.1, 3);
  assert.equal(out[0], 1);
});

console.log(failures ? `\n${failures} failing` : '\nall palette tests passed');
process.exit(failures ? 1 : 0);
