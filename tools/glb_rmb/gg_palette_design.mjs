#!/usr/bin/env node
/*
 * Turn an importer palette report into the two hardware palettes the machine
 * actually loads, plus their temporal-interleave partners.
 *
 * The single most useful fact about colour on this hardware is that the hero
 * and the room do not compete for it. The hero is drawn as sprites and reads
 * the 16-entry SPRITE palette; the room is background tiles and reads the two
 * 16-entry BG palettes, selected per tile by the name-table bit the ambient/
 * lit contract already uses. So a warm figure standing in a cool room is not a
 * compromise between two halves of one palette -- it is two independent
 * palettes, and it costs nothing beyond the 32 bytes of palette RAM that were
 * always going to be written anyway.
 *
 * That is worth spending deliberately. The room gets a desaturated cool ramp
 * with an aerial-perspective gradient: the compositor's wall semantics are
 * literally distance bands (FAR, FAR_MID, MID, MID_NEAR, NEAR), so shifting
 * the far end bluer and lower in contrast and the near end warmer and higher
 * in contrast puts a real depth cue into the palette, for free, in a renderer
 * that has no fog and cannot afford one. The hero gets its material's own hue
 * at full strength. The two being complementary is what makes the figure sit
 * in the room instead of on top of it.
 */
import fs from 'node:fs/promises';
import path from 'node:path';
import { srgb8ToOklab, synthesizeRamp, solveRamp, ggToSrgb8, ggToOklab,
         oklabDistance, oklabToLinear, gamutMapOklab, fitRampChroma,
         RAMP_MIN_STEP_L } from './palette.mjs';

function fail(m){ console.error('fatal:',m); process.exit(2); }
function argValue(a,n,d){ const i=a.indexOf(n); return i>=0&&i+1<a.length?a[i+1]:d; }

/* Semantic enum from tools/polar_baked_composite.c. The names are load-bearing
 * here: the enum's VALUES are not in brightness order (the two interstitial
 * stops were appended after the original three), so a palette written in enum
 * order would put the ramp's second-darkest stop where its brightest belongs.
 * That exact confusion already cost this project a 21x tonal-error regression
 * in the tile quantizer; see docs/experiments/HERO_LOD_SHADE_METRIC.md. */
const SEM = { BLACK:0, CEILING:1, FLOOR:2, FAR:3, MID:4, NEAR:5, FAR_MID:6, MID_NEAR:7 };
const BRIGHTNESS_ORDER = [SEM.FAR, SEM.FAR_MID, SEM.MID, SEM.MID_NEAR, SEM.NEAR];

/* Cool room hue, low saturation so it recedes behind the warm figure. */
const ROOM_HUE_SRGB = [96,116,150];
/*
 * Lightness bands, taken from the greyscale design that already ships.
 *
 * This is the correction that matters most in the whole file, and it took
 * looking at a render to see it. The material decides the hero's HUE and
 * SATURATION. It does not get to decide its lightness range, because lightness
 * is what carries SHAPE at this pixel scale -- it is the entire signal the
 * shade ramp exists to deliver, and the tile quantizer's whole objective is
 * defined on it. Handing the material's own dark clay albedo to the ramp put
 * the hero's shadow side below the wall behind it and its lit side into neon;
 * over-correcting the other way, by deriving the band from the room's, squeezed
 * all five stops into a bright quarter of the range and flattened the figure.
 *
 * The band that works is already in the repository. The shipped
 * k_bg_palettes / k_sprite_palette put every ramp stop between Oklab L 0.388
 * and 0.790, at a low, near-constant chroma, with the two non-ramp plane roles
 * (ceiling 0.201, floor 0.260) below them. Those numbers were arrived at by
 * looking at the thing on hardware, and nothing about adding colour makes them
 * wrong.
 *
 * So colour keeps them. The hero gets a little more room at the bottom, since
 * a warm hue against a cool room has a second separation channel the grey
 * version did not have and can afford a darker shadow side. Everything else --
 * every lightness the renderer will put on screen -- is where it already was,
 * which is why the colour build's depth reading cannot regress relative to the
 * greyscale build: it is the same image with hue added.
 */
const ROOM_L_BAND = [0.388, 0.790];
const HERO_L_BAND = [0.355, 0.790];
const CEILING_L = 0.201;
const FLOOR_L = 0.260;
/* Aerial perspective: how much bluer and flatter the far end of the wall ramp
 * is than the near end. Applied as a lightness compression toward the ramp's
 * mid plus a blue push, both strongest at FAR and zero at NEAR. */
/*
 * Aerial perspective, split into its two halves on purpose.
 *
 * The BLUE push is free: it is a chroma change, and chroma is not what carries
 * shape here, so it can be as strong as it wants. The lightness FLATTEN is not
 * free -- it moves a semantic away from where the greyscale design put it, and
 * the whole "colour is the same image with hue added" guarantee is a bound on
 * exactly that displacement. Held at 0.12 so the worst-displaced stop stays
 * inside one ramp step (0.053 against a 0.062 step); at 0.30 the far wall
 * moved 0.071 and broke it, which the CI check caught.
 */
const AERIAL_FLATTEN = 0.12;
const AERIAL_BLUE = 0.028;

function aerialAdjust(ramp){
  const mid = ramp[Math.floor(ramp.length/2)][0];
  return ramp.map((lab,i)=>{
    const far = 1 - i/(ramp.length-1);          /* 1 at FAR, 0 at NEAR */
    const L = lab[0] + (mid - lab[0]) * AERIAL_FLATTEN * far;
    return gamutMapOklab(L, lab[1], lab[2] - AERIAL_BLUE * far);
  });
}

const args=process.argv.slice(2);
if(args.length<2) fail('usage: gg_palette_design.mjs IMPORT_PALETTE.json OUT_PREFIX '+
  '[--room-hue r,g,b] [--hero-chroma 1.0] [--room-chroma 1.0]');
const report=JSON.parse(await fs.readFile(args[0],'utf8'));
const outPrefix=args[1];
const roomHue=String(argValue(args,'--room-hue',ROOM_HUE_SRGB.join(','))).split(',').map(Number);
if(roomHue.length!==3||roomHue.some(v=>!(v>=0&&v<=255))) fail('--room-hue must be r,g,b in 0..255');
/* Chroma is the one honest taste knob in here. Everything else -- the
 * lightness bands, the aerial gradient, the interleave pairs -- is derived or
 * measured; how saturated the piece should look is not something a metric can
 * settle, so it is exposed rather than hidden inside a constant. Both default
 * to the material's own strength, capped only where the ramp would lose tonal
 * resolution (see fitRampChroma). */
/* 0.72, not 1.0. The material's measured saturation is the one number in this
 * pipeline that came out too strong on screen: at full strength the figure
 * reads as molten rather than as fired clay, and its internal shading is
 * harder to follow even though the lightness structure is provably intact
 * (see LIGHTNESS_DELTA in tools/recolour_semantic_frames.py). Chosen by
 * rendering 1.00 / 0.72 / 0.50 and looking at them; the sweep is committed as
 * docs/experiments/images/doomguy-colour-chroma-sweep.png so the choice can be
 * argued with rather than taken on trust. */
const heroChroma=Number(argValue(args,'--hero-chroma','0.72'));
const roomChroma=Number(argValue(args,'--room-chroma','1.0'));
if(!(heroChroma>=0&&heroChroma<=2)||!(roomChroma>=0&&roomChroma<=2))
  fail('--hero-chroma/--room-chroma must be in 0..2');

const polychrome=report.layout==='family-split';
if(!['mono-ramp','family-split'].includes(report.layout))
  fail(`unknown palette layout ${report.layout}`);
if(!polychrome&&report.entries.length!==1)
  fail('mono-ramp report must carry exactly one family');
for(const e of report.entries)
  if(e.stops.length!==BRIGHTNESS_ORDER.length)
    fail(`every family needs ${BRIGHTNESS_ORDER.length} stops to map onto the `+
         `compositor's brightness ramp; family ${e.family} has ${e.stops.length}`);
/*
 * Sprite colour 0 is transparent on this hardware, so the hero has 15 usable
 * entries and 3 families x 5 shades fills them exactly. That is the layout,
 * and it is chosen over the tidier 4x4 on measurement: this asset's materials
 * come out at roughly 50% / 44% / 6% of surface area, so a fourth family would
 * be spent on well under a percent while costing every material a shade stop
 * -- and shade stops are what carry shape.
 */
if(polychrome&&report.entries.length*BRIGHTNESS_ORDER.length>15)
  fail(`${report.entries.length} families x ${BRIGHTNESS_ORDER.length} shades `+
       `exceeds the 15 usable sprite palette entries`);
const hero=report.entries[0];

/* ---- Room ---------------------------------------------------------------
 * Same ramp machinery as the hero, so both surfaces are lit by the same rules
 * and the room cannot drift into a different rendering convention. */
const roomLab0 = srgb8ToOklab(roomHue[0],roomHue[1],roomHue[2]);
const roomFit = fitRampChroma(roomLab0, BRIGHTNESS_ORDER.length, ROOM_L_BAND);
const roomScale = roomFit.scale*roomChroma;
const roomLab = [roomLab0[0], roomLab0[1]*roomScale, roomLab0[2]*roomScale];
const roomRamp = aerialAdjust(
  synthesizeRamp(roomLab, BRIGHTNESS_ORDER.length, null, ROOM_L_BAND));
const roomSolved = solveRamp(roomRamp);

/* CEILING and FLOOR are not on the brightness ramp; they are the two plane
 * roles. The ceiling sits just under the darkest wall stop so it closes the
 * box. The floor takes a small amount of the hero's warmth -- it is the
 * surface the figure stands on and the only one that receives bounce from it
 * -- but only a small amount: enough to tie the two palettes together, not
 * enough to turn the ground plane muddy, which is what a larger share does. */
const heroLab = hero.centerOklab;
const ceilLab = gamutMapOklab(CEILING_L, roomRamp[0][1]*0.70, roomRamp[0][2]*0.70-0.010);
const floorLab = gamutMapOklab(FLOOR_L,
                               roomRamp[0][1]*0.75 + heroLab[1]*0.10,
                               roomRamp[0][2]*0.75 + heroLab[2]*0.10);
const ceilSolved = solveRamp([ceilLab])[0];
const floorSolved = solveRamp([floorLab])[0];

/* ---- Hero, placed in that room -------------------------------------------
 * Re-synthesized here rather than reused from the importer's report: the
 * importer measured the material without knowing what scene it would stand in,
 * and the lightness band is the part that belongs to the scene. */
const heroBand = HERO_L_BAND;
/*
 * Every family gets the SAME lightness band. That is deliberate and it is the
 * whole reason a family plane is safe to add: shade already says how lit a
 * pixel is, so if a green leaf and a red petal at the same shade level sat at
 * different lightnesses, the family plane would be smuggling shading
 * information into the colour channel and the figure's form would depend on
 * which material happened to be facing the light. Families differ in hue and
 * saturation. They do not differ in lightness.
 *
 * Chroma is fitted per family, though, because the cap is a per-hue property:
 * a saturated green rails its green channel at a different lightness than a
 * saturated red rails its red one.
 */
const heroFamilies = report.entries.map(fam=>{
  const lab = fam.centerOklab;
  const fit = fitRampChroma(lab, BRIGHTNESS_ORDER.length, heroBand);
  const scale = fit.scale*heroChroma;
  const ramp = synthesizeRamp([lab[0], lab[1]*scale, lab[2]*scale],
                              BRIGHTNESS_ORDER.length, null, heroBand);
  return {family:fam.family, areaShare:fam.areaShare,
          shellAreaShare:fam.shellAreaShare ?? fam.areaShare,
          hueDeg:fam.hueDeg, saturation:fam.saturation,
          fittedScale:fit.scale, appliedScale:scale,
          worstAdjacentGapL:fit.worstGapL,
          solved:solveRamp(ramp)};
});
const heroSolved = heroFamilies[0].solved;
const heroFit = {scale:heroFamilies[0].fittedScale,
                 worstGapL:heroFamilies[0].worstAdjacentGapL};
const heroScale = heroFamilies[0].appliedScale;

/* ---- Assemble the hardware tables ---------------------------------------
 * BG palette 0 is ambient. BG palette 1 is the same ramp one stop brighter,
 * which is the semantic contract the name-table palette bit already encodes:
 * a tile flagged lit does not get a different colour scheme, it gets the same
 * scheme further up its own ramp. Building palette 1 by SHIFTING palette 0
 * rather than by re-solving it is what keeps that promise exact.
 */
function emptyPalette(){ return Array.from({length:16},()=>[0,0,0]); }
const PICK = {
  static: st=>st.gg,
  a: st=>st.interleave.a,
  b: st=>st.interleave.b
};

/*
 * `pick` selects which of a solved stop's three variants (static, interleave
 * frame A, frame B) this table is being built from. Passing it in keeps the
 * three tables structurally identical -- which matters, because the interleave
 * only works if A and B differ ONLY in the colours and not in which semantic
 * sits where.
 *
 * BG palette 1 is not a second colour scheme, it is the same ramp one stop
 * brighter -- that is exactly what the name-table palette-select bit means in
 * this renderer, and building it by SHIFTING rather than re-solving is what
 * keeps the promise exact. Its upper half carries the `v+7` shadow aliases the
 * compositor emits for the shadow side of a mixed boundary tile: entry v+7 in
 * palette 1 must equal entry v in palette 0, or a boundary tile shows the
 * bright variant on both sides of its own edge.
 */
function buildBG(which,pick){
  const p = emptyPalette();
  const shift = which===1 ? 1 : 0;
  p[SEM.BLACK] = [0,0,0];
  /* The two plane roles are not on the brightness ramp, so "one stop
   * brighter" has to mean something for them explicitly. The shipped table
   * promotes ceiling to the ambient floor colour and floor to the ambient
   * darkest wall -- a ladder of its own -- and that is reproduced here rather
   * than reinvented. */
  p[SEM.CEILING] = which===1 ? pick(floorSolved) : pick(ceilSolved);
  p[SEM.FLOOR] = which===1 ? pick(roomSolved[0]) : pick(floorSolved);
  BRIGHTNESS_ORDER.forEach((sem,i)=>{
    p[sem] = pick(roomSolved[Math.min(roomSolved.length-1, i+shift)]);
  });
  if(which===1){
    const ambient = buildBG(0,pick);
    for(let v=1;v<8;++v) p[v+7] = ambient[v];
    p[15] = [0,0,0];
  }
  return p;
}
/*
 * Sprite colour 0 is transparent on this hardware; nothing may be written
 * there and no hero pixel may resolve to it. The compositor's SEM_BLACK is
 * exactly the "no hero here" code, so the two coincide by construction.
 *
 * mono-ramp writes its five stops at their SEMANTIC indices, so the hero's
 * pixel codes do not change at all -- that is what makes single-material
 * colour free. family-split cannot do that: it needs three ramps and there
 * are only eight semantic values. So it packs families consecutively from
 * index 1 and the pixel value becomes 1 + family*5 + position-on-the-ramp.
 * That renumbering is the real cost of a second material, because it is what
 * makes the tile vocabulary grow -- see HERO_COLOUR_PALETTE.md.
 */
function spriteIndex(family,shadeOrdinal){
  return polychrome ? 1 + family*BRIGHTNESS_ORDER.length + shadeOrdinal
                    : BRIGHTNESS_ORDER[shadeOrdinal];
}
function buildSprite(pickStop){
  const p = emptyPalette();
  heroFamilies.forEach(fam=>{
    fam.solved.forEach((st,i)=>{ p[spriteIndex(fam.family,i)] = pickStop(st); });
  });
  return p;
}

const design = {
  source: path.resolve(args[0]),
  layout: report.layout,
  heroCenterSrgb8: hero.centerSrgb8,
  roomHueSrgb8: roomHue,
  semantics: SEM,
  brightnessOrder: BRIGHTNESS_ORDER,
  bg: { static:[buildBG(0,PICK.static),buildBG(1,PICK.static)],
        interleaveA:[buildBG(0,PICK.a),buildBG(1,PICK.a)],
        interleaveB:[buildBG(0,PICK.b),buildBG(1,PICK.b)] },
  sprite: { static:buildSprite(PICK.static),
            interleaveA:buildSprite(PICK.a),
            interleaveB:buildSprite(PICK.b) },
  heroLBand: heroBand, roomLBand: ROOM_L_BAND,
  polychrome,
  /* Palette index of every (family, ramp position) pair, so a renderer never
   * has to re-derive the packing. */
  spriteIndex: heroFamilies.map(fam=>
    BRIGHTNESS_ORDER.map((_,i)=>spriteIndex(fam.family,i))),
  heroFamilies: heroFamilies.map(f=>({
    family:f.family, areaShare:f.areaShare, shellAreaShare:f.shellAreaShare,
    hueDeg:f.hueDeg, saturation:f.saturation,
    fittedChromaScale:f.fittedScale, appliedChromaScale:f.appliedScale,
    worstAdjacentGapL:f.worstAdjacentGapL,
    stops:f.solved
  })),
  chroma: {
    heroFittedScale: heroFit.scale, heroRequested: heroChroma,
    heroApplied: heroScale, heroWorstAdjacentGapL: heroFit.worstGapL,
    roomFittedScale: roomFit.scale, roomRequested: roomChroma,
    roomApplied: roomScale, roomWorstAdjacentGapL: roomFit.worstGapL,
    minStepL: RAMP_MIN_STEP_L
  },
  hero: { stops: heroSolved, paletteIndices: hero.indices,
          materialStopsAsMeasured: hero.stops },
  room: { wall: roomSolved, ceiling: ceilSolved, floor: floorSolved }
};

/* Accuracy of the whole design, hero and room together, static vs interleaved.
 * Reported for every stop the hardware will actually show, so it is not the
 * importer's hero-only number quoted twice. */
const allStops = [...heroFamilies.flatMap(f=>f.solved), ...roomSolved,
                  ceilSolved, floorSolved];
design.metrics = {
  stops: allStops.length,
  meanStaticErrOklab: allStops.reduce((a,s)=>a+s.staticErr,0)/allStops.length,
  meanInterleavedErrOklab: allStops.reduce((a,s)=>a+s.interleave.err,0)/allStops.length,
  maxLumaSplit: Math.max(...allStops.map(s=>s.interleave.lumaSplit)),
  unsafeStops: allStops.filter(s=>!s.interleave.safe).length
};
design.metrics.interleaveGainPct =
  100*(1-design.metrics.meanInterleavedErrOklab/design.metrics.meanStaticErrOklab);

await fs.mkdir(path.dirname(outPrefix),{recursive:true});
await fs.writeFile(outPrefix+'.json', JSON.stringify(design,null,2),'utf8');

/* ---- C tables ----------------------------------------------------------- */
const rgbMacro = c => `RGB(${c[0]},${c[1]},${c[2]})`;
const table = (name,rows) =>
  `static const palette_color_t ${name}[${rows.length}] = {\n` +
  rows.map((r,i)=>'    '+rgbMacro(r)+(i+1<rows.length?',':'')).join('\n') + '\n};\n';

const inc = `/* Generated by tools/glb_rmb/gg_palette_design.mjs. DO NOT HAND EDIT.
 * source palette report: ${path.basename(args[0])}
 * layout: ${report.layout}, ${heroFamilies.length} material famil${heroFamilies.length===1?'y':'ies'} x ${BRIGHTNESS_ORDER.length} shades
${heroFamilies.map(f=>` *   family ${f.family}: hue ${Math.round(f.hueDeg)}deg, ${(100*f.areaShare).toFixed(1)}% of surface`).join('\n')}
 * hero lightness band: ${heroBand.map(v=>v.toFixed(3)).join('..')} Oklab L,
 *   chroma x${heroScale.toFixed(2)} (tonal-resolution cap allowed ${heroFit.scale.toFixed(2)})
 * room hue: sRGB ${roomHue.join(',')}
 * mean stop error: static ${design.metrics.meanStaticErrOklab.toFixed(4)} Oklab,
 *   temporally interleaved ${design.metrics.meanInterleavedErrOklab.toFixed(4)}
 *   (${design.metrics.interleaveGainPct.toFixed(1)}% closer), max luma split
 *   ${design.metrics.maxLumaSplit.toFixed(2)} of 15
 *
 * Ramp stops are written at their SEMANTIC index, not in ramp order: the
 * compositor's enum appends its two interstitial stops after the original
 * three, so brightness order is 3,6,4,7,5 and a table written 3,4,5,6,7 would
 * be scrambled.
 */
#ifndef HERO_COLOUR_PALETTE_INC
#define HERO_COLOUR_PALETTE_INC

/* Frame-static tables. Load these and nothing else if the interleave is off. */
${table('k_hero_colour_bg', [...design.bg.static[0], ...design.bg.static[1]])}
${table('k_hero_colour_sprite', design.sprite.static)}

/* Temporal interleave: alternate A and B every frame. Cost is one 32-byte
 * palette write per vblank -- the same bandwidth as a SINGLE tile pattern
 * upload, for the whole screen. Perceived colour is the linear-light average
 * of the pair, which is finer than the 4-bit grid can address on its own.
 * Every pair below is within ${design.metrics.maxLumaSplit.toFixed(2)} of one
 * luminance level, so what alternates is chroma, which this panel does not
 * show as flicker. */
${table('k_hero_colour_bg_a', [...design.bg.interleaveA[0], ...design.bg.interleaveA[1]])}
${table('k_hero_colour_bg_b', [...design.bg.interleaveB[0], ...design.bg.interleaveB[1]])}
${table('k_hero_colour_sprite_a', design.sprite.interleaveA)}
${table('k_hero_colour_sprite_b', design.sprite.interleaveB)}

#endif /* HERO_COLOUR_PALETTE_INC */
`;
await fs.writeFile(outPrefix+'.inc', inc, 'utf8');

console.log(JSON.stringify({
  out:{json:path.resolve(outPrefix+'.json'), inc:path.resolve(outPrefix+'.inc')},
  metrics:design.metrics,
  layout:report.layout,
  heroRamps:heroFamilies.map(f=>({family:f.family,
    area:+(100*f.areaShare).toFixed(1), hue:Math.round(f.hueDeg),
    ramp:f.solved.map(s=>s.srgb8)})),
  heroBand, chroma:design.chroma,
  roomRamp:roomSolved.map(s=>s.srgb8),
  ceiling:ceilSolved.srgb8, floor:floorSolved.srgb8
},null,2));
