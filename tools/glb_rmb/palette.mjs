/*
 * Material palette extraction for the baked hero renderer.
 *
 * The renderer draws the hero as 4bpp sprite tiles out of a shared pattern
 * vocabulary. Sprites on this hardware always use the sprite palette, colour 0
 * is transparent, so the entire figure has to live in 15 colours -- and every
 * extra distinct pixel VALUE the compositor can emit is a multiplier on the
 * tile vocabulary, which is the scarce resource. Colour is therefore not a
 * texture problem here, it is a code-alphabet problem.
 *
 * The layout this module solves for is a 2+2 split:
 *
 *     palette index = (family << 2) | shade        family,shade in 0..3
 *
 * chosen so that in a 4bpp planar tile the two SHADE bits land in bitplanes
 * 0-1 and the two FAMILY bits in bitplanes 2-3. Shade is high spatial
 * frequency and family is low (a statue's material regions are large and
 * contiguous), so the two halves compress at wildly different rates and are
 * worth quantizing as separate images. See tools/analyze_hero_colour_codec.py.
 *
 * Shade 0 is the shared void stop: every family collapses to palette index 0
 * there. That is not a compromise, it is the correct perceptual call -- hue
 * discrimination is negligible at the bottom of the ramp -- and it costs the
 * family plane nothing to encode because those pixels are transparent anyway.
 *
 * All colour reasoning happens in Oklab, not RGB. A darker version of a hue is
 * a lightness reduction at near-constant chroma; the naive RGB multiply that
 * "darken the colour" usually means also collapses chroma and rotates hue,
 * which on a 4-bit-per-channel grid turns every shadow into the same brown.
 */

const EPS = 1e-12;

export function srgbToLinear(c) {
  return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
}
export function linearToSrgb(c) {
  if (c <= 0) return 0;
  if (c >= 1) return 1;
  return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
}

/* Ottosson's Oklab. Perceptually uniform enough that a Euclidean distance in
 * it is a defensible clustering metric, which CIELAB is not for saturated
 * blues and CIELUV is not for anything. */
export function linearToOklab(r, g, b) {
  const l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
  const m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
  const s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
  const l_ = Math.cbrt(l), m_ = Math.cbrt(m), s_ = Math.cbrt(s);
  return [
    0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
    1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
    0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_
  ];
}
export function oklabToLinear(L, a, b) {
  const l_ = L + 0.3963377774 * a + 0.2158037573 * b;
  const m_ = L - 0.1055613458 * a - 0.0638541728 * b;
  const s_ = L - 0.0894841775 * a - 1.2914855480 * b;
  const l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
  return [
    4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
    -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
    -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s
  ];
}
export function srgb8ToOklab(r8, g8, b8) {
  return linearToOklab(srgbToLinear(r8 / 255), srgbToLinear(g8 / 255),
                       srgbToLinear(b8 / 255));
}

/* Game Gear colour is 4 bits per channel: 4096 vertices on a uniform grid in
 * sRGB-encoded space (the DAC is not gamma aware, the levels are just voltage
 * steps), so quantization happens after the transfer curve, not before. */
export const GG_LEVELS = 16;

/*
 * Gamut map by chroma reduction at constant lightness and hue.
 *
 * The ramp deliberately pushes chroma up as lightness rises, which for a
 * saturated hue walks straight out of the display gamut. Letting the
 * per-channel clamp inside the quantizer handle that is the standard mistake:
 * clamping red at maximum while green and blue keep climbing does not darken
 * the colour, it ROTATES ITS HUE -- a vivid red highlight silently becomes
 * orange, and on a four-stop ramp that reads as the material changing between
 * shade levels. Backing chroma off instead keeps the hue and spends the error
 * on saturation, which is the invisible axis here.
 */
export function gamutMapOklab(L, a, b) {
  const inside = c => {
    const lin = oklabToLinear(L, a * c, b * c);
    return lin.every(v => v >= -1e-4 && v <= 1 + 1e-4);
  };
  if (inside(1)) return [L, a, b];
  let lo = 0, hi = 1;
  for (let i = 0; i < 24; ++i) {
    const mid = (lo + hi) / 2;
    if (inside(mid)) lo = mid; else hi = mid;
  }
  return [L, a * lo, b * lo];
}

export function ggQuantize(lin) {
  const q = c => Math.max(0, Math.min(15, Math.round(linearToSrgb(c) * 15)));
  return [q(lin[0]), q(lin[1]), q(lin[2])];
}
export function ggToOklab(gg) {
  const f = v => srgbToLinear(v / 15);
  return linearToOklab(f(gg[0]), f(gg[1]), f(gg[2]));
}
export function ggToSrgb8(gg) {
  return gg.map(v => Math.round(v * 255 / 15));
}
export function oklabDistance(p, q) {
  const dL = p[0] - q[0], da = p[1] - q[1], db = p[2] - q[2];
  /* Lightness is weighted up: at this pixel scale a wrong lightness reads as a
   * wrong SHAPE (the silhouette's internal form), a wrong hue only reads as a
   * wrong colour. Shape errors are the ones the eye reports as "broken". */
  return Math.sqrt(2.0 * dL * dL + da * da + db * db);
}

/*
 * Weighted k-means over Oklab with k-means++ seeding and a fixed RNG, so a
 * rebuild of the same asset produces the same families in the same order.
 * Weights are surface area, not vertex count -- a decimated flat skirt panel
 * and a dense filigreed hand must not get equal say in what the palette is.
 */
export function kmeansOklab(lab, weights, k, iterations = 40, seed = 0x5eed1234) {
  const n = weights.length;
  if (n === 0) return { centers: [], assign: new Int32Array(0), inertia: 0 };
  const kk = Math.min(k, n);
  let state = seed >>> 0;
  const rnd = () => {
    state ^= state << 13; state >>>= 0;
    state ^= state >>> 17;
    state ^= state << 5; state >>>= 0;
    return state / 4294967296;
  };
  const centers = [];
  const d2 = new Float64Array(n).fill(Infinity);
  {
    let total = 0;
    for (let i = 0; i < n; ++i) total += weights[i];
    let pick = 0, acc = 0, target = rnd() * total;
    for (let i = 0; i < n; ++i) { acc += weights[i]; if (acc >= target) { pick = i; break; } }
    centers.push([lab[pick * 3], lab[pick * 3 + 1], lab[pick * 3 + 2]]);
    while (centers.length < kk) {
      const c = centers[centers.length - 1];
      let sum = 0;
      for (let i = 0; i < n; ++i) {
        const dx = lab[i * 3] - c[0], dy = lab[i * 3 + 1] - c[1], dz = lab[i * 3 + 2] - c[2];
        const d = 2.0 * dx * dx + dy * dy + dz * dz;
        if (d < d2[i]) d2[i] = d;
        sum += d2[i] * weights[i];
      }
      let acc2 = 0, t = rnd() * sum, pick2 = 0;
      for (let i = 0; i < n; ++i) { acc2 += d2[i] * weights[i]; if (acc2 >= t) { pick2 = i; break; } }
      centers.push([lab[pick2 * 3], lab[pick2 * 3 + 1], lab[pick2 * 3 + 2]]);
    }
  }
  const assign = new Int32Array(n).fill(-1);
  let inertia = 0;
  for (let it = 0; it < iterations; ++it) {
    let moved = 0;
    inertia = 0;
    for (let i = 0; i < n; ++i) {
      let best = 0, bestD = Infinity;
      for (let c = 0; c < centers.length; ++c) {
        const dx = lab[i * 3] - centers[c][0];
        const dy = lab[i * 3 + 1] - centers[c][1];
        const dz = lab[i * 3 + 2] - centers[c][2];
        const d = 2.0 * dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = c; }
      }
      if (assign[i] !== best) { assign[i] = best; ++moved; }
      inertia += bestD * weights[i];
    }
    const sums = centers.map(() => [0, 0, 0, 0]);
    for (let i = 0; i < n; ++i) {
      const s = sums[assign[i]], w = weights[i];
      s[0] += lab[i * 3] * w; s[1] += lab[i * 3 + 1] * w;
      s[2] += lab[i * 3 + 2] * w; s[3] += w;
    }
    for (let c = 0; c < centers.length; ++c)
      if (sums[c][3] > EPS)
        centers[c] = [sums[c][0] / sums[c][3], sums[c][1] / sums[c][3], sums[c][2] / sums[c][3]];
    if (!moved && it > 0) break;
  }
  return { centers, assign, inertia };
}

/*
 * Build one family's shade ramp.
 *
 * The single most important thing here is what the ramp is NOT: it is not a
 * reproduction of the material's albedo. The renderer already hands us a shade
 * level that encodes incident light from "facing away" to "catching it"; this
 * function decides what colour that level should be. Reproducing albedo
 * lightness directly is the obvious thing to do and it is wrong -- this
 * asset's base texture averages sRGB (87,23,8), a dark clay red, and laying
 * four stops around that lightness on a 4-bit-per-channel grid produces four
 * near-black reds that the panel cannot separate. The figure goes muddy and,
 * worse, the tile quantizer is handed an image with less tonal content and
 * dutifully reports a LOWER error while looking flatter.
 *
 * So the material contributes its HUE and its SATURATION; the display budget
 * contributes the lightness span. A family's own lightness only shifts it
 * modestly within that span, enough to keep families in their true relative
 * order without any of them falling off the usable range.
 *
 * Chroma then scales with the lightness being rendered at, not held constant.
 * That is how a real surface behaves -- absolute chroma has nowhere to go as
 * lightness approaches zero -- and it is why a naive RGB multiply looks wrong:
 * holding chroma while dropping lightness gives the plastic look, dropping
 * chroma proportionally gives clay. A gentle extra taper at both ends covers
 * the two physical cases the linear rule misses: shadow filled by achromatic
 * room bounce, and a highlight taking the light source's own colour.
 */

/* Default usable lightness span, used when the caller has no better idea.
 * The bottom is above zero because the very bottom of the ramp is the shared
 * void stop and does not need a colour; the top is below one because the
 * 4-bit grid's last two levels are perceptually almost identical and spending
 * a stop there buys nothing.
 *
 * A caller that knows what the object will be seen AGAINST should override
 * this -- see the hero band in gg_palette_design.mjs. Lightness range is a
 * figure/ground decision, not a material property. */
const RAMP_L_FLOOR = 0.20;
const RAMP_L_CEIL = 0.90;
/* A saturated hue cannot be taken to the top of the lightness range and stay
 * in gamut: chroma has to be given up, and past a point the top stop stops
 * being the material's colour at all and becomes a pale blowout. This is the
 * fraction of the requested chroma the brightest stop must retain; the ceiling
 * is lowered until it does. A physical highlight does desaturate -- it just
 * must not desaturate into white. */
const RAMP_TOP_CHROMA_KEEP = 0.62;
const RAMP_L_GAMMA = 0.85;
/* How much of a family's own lightness survives into its ramp placement. */
const FAMILY_L_SHIFT = 0.30;
/* Chroma shape across the ramp, sampled at t = 0, 1/4, 1/2, 3/4, 1. */
const CHROMA_CURVE = [0.55, 0.85, 1.00, 1.00, 0.88];
/* Cool cast in Oklab b (negative = blue), strongest in shadow, matching the
 * blue-grey shadows the existing neutral room ramp already uses so coloured
 * and neutral surfaces read as being lit by the same room. */
const COOL_CURVE = [0.030, 0.016, 0.006, 0.000, 0.000];

function sampleCurve(curve, t) {
  const x = Math.max(0, Math.min(1, t)) * (curve.length - 1);
  const i = Math.min(curve.length - 2, Math.floor(x));
  return curve[i] + (curve[i + 1] - curve[i]) * (x - i);
}

/*
 * centerLab   family centre in Oklab
 * stops       number of shade levels the renderer will emit
 * meanL       mean family lightness, so families can be placed relative to
 *             each other rather than absolutely
 */
export function synthesizeRamp(centerLab, stops = 4, meanL = null, lRange = null) {
  const [L0, a0, b0] = centerLab;
  const C0 = Math.hypot(a0, b0);
  /* Saturation as chroma per unit lightness: the hue's strength, independent
   * of how dark the artist happened to paint it. */
  const sat = L0 > 1e-4 ? C0 / L0 : 0;
  const hx = C0 > 1e-9 ? a0 / C0 : 0;
  const hy = C0 > 1e-9 ? b0 / C0 : 0;
  const shift = meanL === null ? 0 : FAMILY_L_SHIFT * (L0 - meanL);
  const floor = lRange ? lRange[0] : RAMP_L_FLOOR;
  /* Lower the ceiling until the top stop keeps enough of its chroma. */
  let ceil = lRange ? lRange[1] : RAMP_L_CEIL;
  if (sat > 1e-4) {
    for (let i = 0; i < 40; ++i) {
      const L = Math.max(0.03, Math.min(0.98, ceil + shift));
      const C = sat * L * sampleCurve(CHROMA_CURVE, 1);
      const mapped = gamutMapOklab(L, hx * C, hy * C);
      if (C < 1e-6 || Math.hypot(mapped[1], mapped[2]) >= RAMP_TOP_CHROMA_KEEP * C) break;
      ceil -= 0.02;
      if (ceil <= floor + 0.15) break;
    }
  }
  const out = [];
  for (let s = 0; s < stops; ++s) {
    const t = stops > 1 ? s / (stops - 1) : 1;
    const L = Math.max(0.03, Math.min(0.98,
      floor + (ceil - floor) * Math.pow(t, RAMP_L_GAMMA) + shift));
    const C = sat * L * sampleCurve(CHROMA_CURVE, t);
    out.push(gamutMapOklab(L, hx * C, hy * C - sampleCurve(COOL_CURVE, t)));
  }
  return out;
}

/*
 * How much of the material's saturation the ramp can actually afford.
 *
 * A saturated hue has LESS usable lightness range on a 4-bit-per-channel grid
 * than a neutral does, and this is the trade that decides whether a coloured
 * figure still reads as a shaded solid or as a flat sticker. A neutral ramp
 * moves all three channels together and gets the full 16 levels of tonal
 * resolution. A saturated red rails its red channel early: past that point the
 * only way up is through green and blue, which desaturates, and two adjacent
 * ramp stops end up differing by almost nothing the eye reads as lightness.
 * The figure keeps its colour and loses its form -- and form is the entire
 * thing the shade ramp exists to carry.
 *
 * So chroma is not taken as given. It is reduced until every adjacent pair of
 * QUANTIZED stops is separated by at least `minStepL`, measured after
 * quantization because that is where the collapse happens. The floor is set
 * from the greyscale design that already ships: its tightest adjacent gap is
 * 0.062 in Oklab L, so a colour ramp that holds 0.055 is not giving up
 * anything the grey build had.
 *
 * The retained fraction is reported. It is the honest price of colour on this
 * hardware, and it is a far better thing to spend than tile vocabulary.
 */
export const RAMP_MIN_STEP_L = 0.055;

export function fitRampChroma(centerLab, stops, lRange, minStepL = RAMP_MIN_STEP_L,
                              synth = null) {
  const build = synth || ((lab, n, r) => synthesizeRamp(lab, n, null, r));
  const [L0, a0, b0] = centerLab;
  const worstGap = scale => {
    const ramp = build([L0, a0 * scale, b0 * scale], stops, lRange);
    const gg = quantizeRamp(ramp);
    let worst = Infinity;
    for (let i = 1; i < gg.length; ++i) {
      const gap = ggToOklab(gg[i])[0] - ggToOklab(gg[i - 1])[0];
      if (gap < worst) worst = gap;
    }
    return worst;
  };
  if (worstGap(1) >= minStepL) return { scale: 1, worstGapL: worstGap(1) };
  let lo = 0, hi = 1;
  for (let i = 0; i < 18; ++i) {
    const mid = (lo + hi) / 2;
    if (worstGap(mid) >= minStepL) lo = mid; else hi = mid;
  }
  return { scale: lo, worstGapL: worstGap(lo) };
}

/*
 * Is this asset actually polychrome?
 *
 * Answering honestly matters more than the palette itself. If the material
 * clusters differ only in lightness -- which is exactly what a single-material
 * scan or an image-to-3D output looks like -- then splitting the palette into
 * families spends bitplanes encoding a distinction that does not exist, and
 * the correct layout is one long ramp in the one hue the asset has. The test
 * is the spread of the family centres in the CHROMA plane only, deliberately
 * ignoring lightness, measured against the size of one 4-bit grid step so the
 * threshold means something physical rather than being a tuned constant.
 */
export function chromaGridStep() {
  /* Distance across one level of the least significant chroma-bearing channel,
   * measured at mid lightness where the grid is finest. */
  const a = ggToOklab([8, 4, 2]), b = ggToOklab([8, 5, 2]);
  return Math.hypot(a[1] - b[1], a[2] - b[2]);
}
/*
 * Chroma that is PROPORTIONAL TO LIGHTNESS is baked shading of one material,
 * not a second material.
 *
 * This is the whole test, and getting it right is what stops the tool from
 * inventing a colour palette an asset does not have. Image-to-3D assets (this
 * one included) arrive with the lighting already painted into the base colour
 * texture, so clustering albedo finds the LIT and the SHADOWED parts of a
 * single clay surface and reports them as four materials. Their chroma differs
 * -- genuinely, measurably -- but only because a darker patch of one material
 * has proportionally less chroma. Splitting the palette on that spends two
 * tile bitplanes re-encoding the shade information the renderer already
 * computes far better from actual geometry.
 *
 * So: fit chroma as a vector proportional to lightness through the origin, and
 * measure what is LEFT. A vector fit, not a scalar one, because a blue family
 * and a red family of equal saturation would leave no scalar residual at all.
 * What remains after removing the proportional part is chroma that lightness
 * cannot explain, which is the definition of a second material. Compared
 * against one 4-bit grid step, so the decision is a property of the hardware.
 */
export function materialResidual(centers, weights) {
  const total = weights.reduce((x, y) => x + y, 0) || 1;
  let sal = 0, sbl = 0, sll = 0;
  for (let i = 0; i < centers.length; ++i) {
    const [L, a, b] = centers[i], w = weights[i];
    sal += a * L * w; sbl += b * L * w; sll += L * L * w;
  }
  if (sll < 1e-12) return { residual: 0, ka: 0, kb: 0 };
  const ka = sal / sll, kb = sbl / sll;
  let v = 0;
  for (let i = 0; i < centers.length; ++i) {
    const [L, a, b] = centers[i], w = weights[i];
    const da = a - ka * L, db = b - kb * L;
    v += (da * da + db * db) * w;
  }
  return { residual: Math.sqrt(v / total), ka, kb };
}

/*
 * Quantize a ramp to the hardware grid and force it to stay monotone and
 * distinct.
 *
 * Not cosmetic. The 4-bit grid is uniform in signal and wildly non-uniform
 * perceptually: levels 0,1,2 are enormous lightness steps and 12..15 are
 * nearly indistinguishable. Adjacent synthesized stops routinely land on the
 * same grid point at the dark end, which silently deletes half the ramp. This
 * is the "not enough colour left to make a darker version of it" case, and the
 * repair is to separate the stops along the family's own hue -- bumping the
 * channel that is already largest first -- rather than toward white, so the
 * family keeps its identity as the ramp opens up.
 */
export function quantizeRamp(ramp) {
  const gg = ramp.map(lab => {
    const m = gamutMapOklab(lab[0], lab[1], lab[2]);
    return ggQuantize(oklabToLinear(m[0], m[1], m[2]));
  });
  const lum = c => 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
  for (let s = 1; s < gg.length; ++s) {
    let guard = 0;
    while (lum(gg[s]) <= lum(gg[s - 1]) + 0.35 && guard++ < 24) {
      const order = [0, 1, 2].sort((p, q) => gg[s][q] - gg[s][p]);
      let bumped = false;
      for (const ch of order) if (gg[s][ch] < 15) { gg[s][ch] += 1; bumped = true; break; }
      if (!bumped) {
        const below = gg[s - 1];
        const ord2 = [0, 1, 2].sort((p, q) => below[p] - below[q]);
        let pulled = false;
        for (const ch of ord2) if (below[ch] > 0) { below[ch] -= 1; pulled = true; break; }
        if (!pulled) break;
      }
    }
  }
  return gg;
}

/*
 * Temporal interleave.
 *
 * Palette RAM is 32 bytes and is rewritten during vblank for free, while one
 * tile pattern upload is 32 bytes for ONE tile. Alternating the palette
 * between two settings every frame is therefore the cheapest expressive knob
 * on the machine: it costs a single tile's worth of upload budget, once, for
 * the whole screen forever.
 *
 * What it buys is colour RESOLUTION, not more shade stops. The panel and the
 * eye integrate the two frames, so an entry can sit BETWEEN two points of the
 * 4-bit grid and the addressable set goes from 4096 colours to roughly 4096
 * squared. That matters most exactly where a short ramp fails -- the dark end,
 * where the grid's steps are perceptually enormous and banding is the visible
 * failure -- and it is what lets a chromatic stop be blended with a neutral to
 * produce the desaturated shadow the grid cannot express on its own.
 *
 * The cost is flicker, and flicker is a LUMINANCE phenomenon: alternating
 * chroma at 60Hz on this panel is invisible, alternating luminance by more
 * than about one grid level is not. So the search below is constrained by
 * luminance split rather than by total colour difference, which lets it reach
 * for large chroma differences (a red and a grey) while refusing small
 * lightness ones. The panel's slow pixel response works in our favour here and
 * is not being relied on: the constraint holds on an ideal display too.
 */
const FLICKER_LUMA_LIMIT = 1.15;
const GG_LUMA = c => 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];

function ggNeighbourhood(targetLab, radius) {
  const m = gamutMapOklab(targetLab[0], targetLab[1], targetLab[2]);
  const lin = oklabToLinear(m[0], m[1], m[2]);
  const c0 = [0, 1, 2].map(i => Math.max(0, Math.min(15,
    Math.round(Math.max(0, Math.min(1, linearToSrgb(lin[i]))) * 15))));
  const out = [];
  for (let r = -radius; r <= radius; ++r)
    for (let g = -radius; g <= radius; ++g)
      for (let b = -radius; b <= radius; ++b) {
        const v = [c0[0] + r, c0[1] + g, c0[2] + b];
        if (v.every(x => x >= 0 && x <= 15)) out.push(v);
      }
  return out;
}

/* Nearest single hardware colour to a target. This is the honest baseline the
 * interleaved result has to beat -- comparing interleave against a value that
 * was moved by some later constraint would flatter it. */
export function bestSingle(targetLab, radius = 2) {
  let best = null, bestErr = Infinity;
  for (const v of ggNeighbourhood(targetLab, radius)) {
    const e = oklabDistance(ggToOklab(v), targetLab);
    if (e < bestErr) { bestErr = e; best = v; }
  }
  return { gg: best, err: bestErr };
}

/* Perceived colour of an interleaved pair. The panel integrates over the two
 * frames, so the blend happens in LINEAR light, not in the 4-bit index space:
 * averaging the indices would be a gamma error of exactly the kind that makes
 * flicker-blended palettes come out too dark. */
export function interleavedOklab(a, b) {
  const f = v => srgbToLinear(v / 15);
  return linearToOklab((f(a[0]) + f(b[0])) / 2, (f(a[1]) + f(b[1])) / 2,
                       (f(a[2]) + f(b[2])) / 2);
}

export function bestPair(targetLab, radius = 2, lumaLimit = FLICKER_LUMA_LIMIT) {
  const cand = ggNeighbourhood(targetLab, radius);
  let best = null, bestErr = Infinity;
  for (let i = 0; i < cand.length; ++i)
    for (let j = i; j < cand.length; ++j) {
      const split = Math.abs(GG_LUMA(cand[i]) - GG_LUMA(cand[j]));
      if (split > lumaLimit) continue;
      const e = oklabDistance(interleavedOklab(cand[i], cand[j]), targetLab);
      if (e < bestErr) { bestErr = e; best = [cand[i], cand[j]]; }
    }
  if (!best) { const s = bestSingle(targetLab, radius); return { a: s.gg, b: s.gg, err: s.err, lumaSplit: 0 }; }
  return { a: best[0], b: best[1], err: bestErr,
           lumaSplit: Math.abs(GG_LUMA(best[0]) - GG_LUMA(best[1])) };
}

/*
 * Solve a whole ramp both ways.
 *
 * Both variants get the same monotone-luminance repair afterwards, for the
 * same reason and at the same strength, so the comparison between them is
 * about colour accuracy and not about one of them having been allowed to
 * break the ramp.
 */
export function solveRamp(ramp, { radius = 2, lumaLimit = FLICKER_LUMA_LIMIT } = {}) {
  const singles = ramp.map(t => bestSingle(t, radius));
  const pairs = ramp.map(t => bestPair(t, radius, lumaLimit));
  const staticGG = quantizeRamp(ramp);
  return ramp.map((t, s) => ({
    shade: s,
    target: t,
    gg: staticGG[s],
    srgb8: ggToSrgb8(staticGG[s]),
    staticErr: oklabDistance(ggToOklab(staticGG[s]), t),
    nearestErr: singles[s].err,
    interleave: {
      a: pairs[s].a, b: pairs[s].b,
      srgb8: ggToSrgb8(pairs[s].a).map((v, k) => Math.round((v + ggToSrgb8(pairs[s].b)[k]) / 2)),
      lumaSplit: pairs[s].lumaSplit,
      safe: pairs[s].lumaSplit <= lumaLimit,
      err: pairs[s].err
    }
  }));
}

/*
 * Chromaticity: chroma per unit lightness.
 *
 * This is the space material families must be found in, and using it rather
 * than Oklab directly is the difference between finding this asset's materials
 * and not finding them. Lightness is the SHADING channel -- the renderer
 * computes its own incident light, ambient occlusion and creases, and
 * --shading-source hybrid already folds the texture's own local darkness into
 * that. A family that meant "dark" would therefore double-count shadow: the
 * pixel would be darkened once by the shade ramp and again by being handed a
 * dark palette entry. What is left for a family to carry is precisely what
 * shading cannot express, which is which material the surface is.
 *
 * Measured on the diorama asset: clustering full Oklab needs K=5 before the
 * green foliage appears as its own cluster at all, because the first four
 * clusters spend themselves splitting the red by lightness. Clustering
 * chromaticity finds red / warm-neutral / green at K=3, stably.
 */
export function chromaticity(lab, floorL = 0.02) {
  const L = Math.max(lab[0], floorL);
  return [lab[1] / L, lab[2] / L];
}

/*
 * How much a sample's chromaticity should be trusted.
 *
 * A near-black texel's hue is quantization noise amplified by the division
 * above, so it must be allowed to be assigned to a family but never to decide
 * where a family sits. Confidence rises with lightness and with how far the
 * sample is from neutral -- a dark grey texel tells you nothing about hue
 * twice over.
 */
export function chromaConfidence(lab, knee = 0.18) {
  const L = Math.max(lab[0], 1e-6);
  const sat = Math.hypot(lab[1], lab[2]) / L;
  return Math.min(1, L / knee) * Math.min(1, sat / 0.10);
}

/*
 * Cluster samples into material families on chromaticity alone.
 *
 * Reuses the weighted Oklab k-means by parking the chromaticity in the a/b
 * slots and holding L constant, so the lightness term of its distance
 * contributes nothing and there is only one clustering implementation to keep
 * correct. Returns families ordered by the area they cover, descending, so
 * family 0 is always the dominant material.
 */
export function clusterFamilies(labs, areas, k, iterations = 40) {
  const n = areas.length;
  const feat = new Float64Array(n * 3);
  const w = new Float64Array(n);
  for (let i = 0; i < n; ++i) {
    const lab = [labs[i * 3], labs[i * 3 + 1], labs[i * 3 + 2]];
    const c = chromaticity(lab);
    feat[i * 3] = 0.5; feat[i * 3 + 1] = c[0]; feat[i * 3 + 2] = c[1];
    w[i] = areas[i] * chromaConfidence(lab);
  }
  const km = kmeansOklab(feat, w, k, iterations);
  const cover = km.centers.map(() => 0);
  for (let i = 0; i < n; ++i) cover[km.assign[i]] += areas[i];
  const rank = km.centers.map((_, c) => c).sort((a, b) => cover[b] - cover[a]);
  const remap = new Int32Array(km.centers.length);
  rank.forEach((oldIdx, newIdx) => { remap[oldIdx] = newIdx; });
  const assign = new Int32Array(n);
  for (let i = 0; i < n; ++i) assign[i] = remap[km.assign[i]];
  return {
    assign,
    /* Each family's chromaticity centre, and the mean lightness of the
     * samples in it -- reported, not used for the ramp, since the ramp's
     * lightness band is a scene decision. */
    families: rank.map((c, f) => {
      let area = 0, sumL = 0;
      for (let i = 0; i < n; ++i) if (assign[i] === f) { area += areas[i]; sumL += labs[i * 3] * areas[i]; }
      return {
        chromaticity: [km.centers[c][1], km.centers[c][2]],
        saturation: Math.hypot(km.centers[c][1], km.centers[c][2]),
        hueDeg: (Math.atan2(km.centers[c][2], km.centers[c][1]) * 180 / Math.PI + 360) % 360,
        area, meanL: area > 0 ? sumL / area : 0
      };
    })
  };
}

/*
 * Transfer a CATEGORICAL field from source vertices onto the shell.
 *
 * The scalar transfer in recess.mjs averages, and averaging a family index is
 * meaningless -- halfway between "red petal" and "green leaf" is not a
 * material. Worse, averaging the underlying COLOUR (which is what the first
 * version of this pipeline did) produces the brown that lies between them,
 * which exists nowhere on the model and which is exactly why this asset first
 * measured as a single muddy hue.
 *
 * So this takes the area-weighted majority within the radius. Ties go to the
 * lower family index, which is the more common material, because the failure
 * that matters is a lone leaf vertex turning a petal green rather than the
 * reverse.
 */
export function transferFamilyToShell(srcXyz, srcFamily, srcWeight, srcCount,
                                      dstXyz, dstCount, radius, familyCount) {
  let mnx = Infinity, mny = Infinity, mnz = Infinity;
  let mxx = -Infinity, mxy = -Infinity, mxz = -Infinity;
  for (let i = 0; i < srcCount; ++i) {
    const x = srcXyz[i * 3], y = srcXyz[i * 3 + 1], z = srcXyz[i * 3 + 2];
    if (x < mnx) mnx = x; if (y < mny) mny = y; if (z < mnz) mnz = z;
    if (x > mxx) mxx = x; if (y > mxy) mxy = y; if (z > mxz) mxz = z;
  }
  const cell = Math.max(radius, 1e-6);
  const gx = Math.max(1, Math.ceil((mxx - mnx) / cell) + 1);
  const gy = Math.max(1, Math.ceil((mxy - mny) / cell) + 1);
  const gz = Math.max(1, Math.ceil((mxz - mnz) / cell) + 1);
  const nCells = gx * gy * gz;
  const counts = new Int32Array(nCells + 1);
  const key = new Int32Array(srcCount);
  const ci = (x, y, z) => {
    let i = Math.floor((x - mnx) / cell), j = Math.floor((y - mny) / cell), k = Math.floor((z - mnz) / cell);
    if (i < 0) i = 0; if (j < 0) j = 0; if (k < 0) k = 0;
    if (i >= gx) i = gx - 1; if (j >= gy) j = gy - 1; if (k >= gz) k = gz - 1;
    return (k * gy + j) * gx + i;
  };
  for (let i = 0; i < srcCount; ++i) {
    const c = ci(srcXyz[i * 3], srcXyz[i * 3 + 1], srcXyz[i * 3 + 2]);
    key[i] = c; counts[c + 1]++;
  }
  for (let c = 0; c < nCells; ++c) counts[c + 1] += counts[c];
  const order = new Int32Array(srcCount);
  const fill = counts.slice(0, nCells);
  for (let i = 0; i < srcCount; ++i) order[fill[key[i]]++] = i;

  const out = new Uint8Array(dstCount);
  const r2 = radius * radius;
  const vote = new Float64Array(familyCount);
  const maxRing = Math.max(gx, gy, gz);
  for (let d = 0; d < dstCount; ++d) {
    const px = dstXyz[d * 3], py = dstXyz[d * 3 + 1], pz = dstXyz[d * 3 + 2];
    /* Clamped into the grid, exactly as source points are when they are
     * binned. Without this a shell vertex outside the source's bounding box
     * indexes cells that do not exist, every lookup misses, and it silently
     * receives the default family -- which on an asset where decimation
     * pushes a vertex past the source hull would paint an unmatched region
     * with the dominant material and look entirely plausible. */
    const i0 = Math.min(gx - 1, Math.max(0, Math.floor((px - mnx) / cell)));
    const j0 = Math.min(gy - 1, Math.max(0, Math.floor((py - mny) / cell)));
    const k0 = Math.min(gz - 1, Math.max(0, Math.floor((pz - mnz) / cell)));
    vote.fill(0);
    let any = false, best = 0, bestD = Infinity;
    for (let k = k0 - 1; k <= k0 + 1; ++k) { if (k < 0 || k >= gz) continue;
    for (let j = j0 - 1; j <= j0 + 1; ++j) { if (j < 0 || j >= gy) continue;
    for (let i = i0 - 1; i <= i0 + 1; ++i) { if (i < 0 || i >= gx) continue;
      const c = (k * gy + j) * gx + i;
      for (let s = counts[c]; s < counts[c + 1]; ++s) {
        const v = order[s];
        const dx = srcXyz[v * 3] - px, dy = srcXyz[v * 3 + 1] - py, dz = srcXyz[v * 3 + 2] - pz;
        const dd = dx * dx + dy * dy + dz * dz;
        if (dd < bestD) { bestD = dd; best = srcFamily[v]; }
        if (dd <= r2) { vote[srcFamily[v]] += srcWeight[v]; any = true; }
      }
    }}}
    if (!any) {
      /* Nothing within the radius. Fall back to the nearest source vertex --
       * but "nearest" has to mean nearest anywhere, not nearest in the cells
       * already looked at. A shell vertex that decimation pushed outside the
       * source hull can sit several cells away from any source, and returning
       * a default family there would silently paint an unmatched region with
       * the dominant material. Widen the ring until something is found; on
       * real data this never runs, and when it does it is cheap because it
       * stops at the first non-empty ring. */
      for (let ring = 2; !Number.isFinite(bestD) && ring <= maxRing; ++ring) {
        for (let k = k0 - ring; k <= k0 + ring; ++k) { if (k < 0 || k >= gz) continue;
        for (let j = j0 - ring; j <= j0 + ring; ++j) { if (j < 0 || j >= gy) continue;
        for (let i = i0 - ring; i <= i0 + ring; ++i) { if (i < 0 || i >= gx) continue;
          /* Only the shell of the ring; the interior was covered already. */
          if (Math.abs(k - k0) !== ring && Math.abs(j - j0) !== ring &&
              Math.abs(i - i0) !== ring) continue;
          const c = (k * gy + j) * gx + i;
          for (let s = counts[c]; s < counts[c + 1]; ++s) {
            const v = order[s];
            const dx = srcXyz[v * 3] - px, dy = srcXyz[v * 3 + 1] - py, dz = srcXyz[v * 3 + 2] - pz;
            const dd = dx * dx + dy * dy + dz * dz;
            if (dd < bestD) { bestD = dd; best = srcFamily[v]; }
          }
        }}}
      }
      out[d] = best;
      continue;
    }
    let win = 0;
    for (let f = 1; f < familyCount; ++f) if (vote[f] > vote[win]) win = f;
    out[d] = vote[win] > 0 ? win : best;
  }
  return out;
}
