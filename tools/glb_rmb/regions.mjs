/*
 * Authored material regions, for assets that carry no material information.
 *
 * The texture path (clusterFamilies + transferFamilyToShell in palette.mjs) is
 * the one to prefer whenever an asset has a texture: it MEASURES which
 * material each part of the surface is, and a measurement can be checked. This
 * file is what to do when there is nothing to measure.
 *
 * FullDoomguyclassic-single-notex.glb is that case and it is worth being
 * precise about how completely: one glTF material with a flat grey
 * baseColorFactor of 0.604, no texture, no image, no COLOR_0, POSITION and
 * NORMAL only, and -- checked rather than assumed -- a single connected
 * component after welding, so not even a sub-mesh split to key off. There is
 * no signal in the file that distinguishes the helmet from the shoulder.
 *
 * Given that, the honest options are to invent a segmentation from geometry
 * and present it as measurement, or to author one and say so. This is the
 * second. A region file is a few geometric predicates in the imported model's
 * own coordinates; it is short, a human can read it, and when it is wrong it
 * is wrong visibly and in one place rather than diffusely inside a heuristic.
 *
 * Coordinates are the importer's normalized space -- Z up, XY centred on the
 * model's bounding box, Z zero at its base, scaled so the model is exactly
 * --height tall. That is the same space the emitted Q8 vertices are in
 * (they are this divided by 256), so a region can be authored by reading
 * numbers off the mesh and checked by rendering it.
 */

function fail(m) { throw new Error(`region spec: ${m}`); }

function num(v, what) {
  if (typeof v !== 'number' || !Number.isFinite(v)) fail(`${what} must be a finite number`);
  return v;
}

function vec3(v, what) {
  if (!Array.isArray(v) || v.length !== 3) fail(`${what} must be [x,y,z]`);
  return v.map((c, i) => num(c, `${what}[${i}]`));
}

const AXIS = { x: 0, y: 1, z: 2 };

/* Each shape is compiled once into a point predicate, so assignment is a
 * single pass over the vertices and the per-shape cost is not paid per vertex
 * in parsing. */
function compileShape(r, where) {
  switch (r.type) {
    case 'halfspace': {
      const axis = AXIS[r.axis];
      if (axis === undefined) fail(`${where}: halfspace axis must be x, y or z`);
      const lo = r.min === undefined ? -Infinity : num(r.min, `${where}.min`);
      const hi = r.max === undefined ? Infinity : num(r.max, `${where}.max`);
      if (!(lo < hi)) fail(`${where}: min must be below max`);
      if (lo === -Infinity && hi === Infinity)
        fail(`${where}: a halfspace with neither min nor max selects everything`);
      return p => p[axis] >= lo && p[axis] <= hi;
    }
    case 'box': {
      const lo = vec3(r.min, `${where}.min`), hi = vec3(r.max, `${where}.max`);
      for (let a = 0; a < 3; ++a)
        if (!(lo[a] < hi[a])) fail(`${where}: min[${a}] must be below max[${a}]`);
      return p => p[0] >= lo[0] && p[0] <= hi[0] &&
                  p[1] >= lo[1] && p[1] <= hi[1] &&
                  p[2] >= lo[2] && p[2] <= hi[2];
    }
    case 'sphere': {
      const c = vec3(r.center, `${where}.center`);
      const rad = num(r.radius, `${where}.radius`);
      if (!(rad > 0)) fail(`${where}: radius must be positive`);
      const r2 = rad * rad;
      return p => {
        const dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        return dx * dx + dy * dy + dz * dz <= r2;
      };
    }
    case 'capsule': {
      /* The one shape that earns its place beyond boxes and spheres: a gun
       * held across the body, or a limb, is a segment with a radius and is
       * hopeless to bound with axis-aligned anything. */
      const a = vec3(r.a, `${where}.a`), b = vec3(r.b, `${where}.b`);
      const rad = num(r.radius, `${where}.radius`);
      if (!(rad > 0)) fail(`${where}: radius must be positive`);
      const ab = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
      const len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
      if (!(len2 > 0)) fail(`${where}: capsule endpoints must differ`);
      const r2 = rad * rad;
      return p => {
        const ap = [p[0] - a[0], p[1] - a[1], p[2] - a[2]];
        let t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        const dx = ap[0] - ab[0] * t, dy = ap[1] - ab[1] * t, dz = ap[2] - ab[2] * t;
        return dx * dx + dy * dy + dz * dz <= r2;
      };
    }
    default:
      fail(`${where}: unknown region type ${JSON.stringify(r.type)}`);
  }
}

export function loadRegionSpec(raw) {
  if (!raw || typeof raw !== 'object') fail('must be a JSON object');
  const families = raw.families;
  if (!Array.isArray(families) || families.length < 1)
    fail('needs a "families" array with at least one entry');
  if (families.length > 15)
    fail(`${families.length} families cannot fit the 15 usable sprite palette entries`);
  const compiled = families.map((f, i) => {
    if (!f || typeof f !== 'object') fail(`families[${i}] must be an object`);
    const srgb = vec3(f.srgb, `families[${i}].srgb`);
    for (const c of srgb)
      if (!(c >= 0 && c <= 255)) fail(`families[${i}].srgb must be 0..255`);
    const off = f.lightnessOffset === undefined ? 0
      : num(f.lightnessOffset, `families[${i}].lightnessOffset`);
    return { index: i, name: String(f.name ?? `family${i}`), srgb, lightnessOffset: off };
  });

  const defaultFamily = raw.default === undefined ? 0 : num(raw.default, 'default');
  if (!(defaultFamily >= 0 && defaultFamily < families.length))
    fail(`default family ${defaultFamily} is outside 0..${families.length - 1}`);

  const regions = Array.isArray(raw.regions) ? raw.regions : [];
  if (!regions.length) fail('needs at least one entry in "regions"');
  const shapes = regions.map((r, i) => {
    const where = `regions[${i}]`;
    if (!r || typeof r !== 'object') fail(`${where} must be an object`);
    const family = num(r.family, `${where}.family`);
    if (!(family >= 0 && family < families.length))
      fail(`${where}.family ${family} is outside 0..${families.length - 1}`);
    return {
      index: i,
      name: String(r.name ?? `${r.type}${i}`),
      family,
      inside: compileShape(r, where)
    };
  });

  return { families: compiled, defaultFamily, regions: shapes };
}

/*
 * Assign one family per vertex.
 *
 * Later regions win. That is the whole overlap rule, and it is enough: it
 * makes subtraction free (a later region naming the default family carves a
 * hole in an earlier one) without a second concept, and it makes the file read
 * top to bottom like a painter working from broad strokes to corrections.
 */
export function assignFamilies(spec, pos, vertexCount) {
  const out = new Uint8Array(vertexCount);
  const hits = spec.regions.map(() => 0);
  const p = [0, 0, 0];
  out.fill(spec.defaultFamily);
  for (let i = 0; i < vertexCount; ++i) {
    p[0] = pos[i * 3]; p[1] = pos[i * 3 + 1]; p[2] = pos[i * 3 + 2];
    for (let r = 0; r < spec.regions.length; ++r) {
      if (spec.regions[r].inside(p)) { out[i] = spec.regions[r].family; hits[r]++; }
    }
  }
  return { family: out, hits };
}

/*
 * Area share per family, and how many vertices each region claimed.
 *
 * Reported rather than assumed, for the same reason the texture path reports
 * its shell-triangle coherence: a region file that misses its target, or that
 * is entirely shadowed by a later one, produces a perfectly valid-looking
 * build. The share is the cheapest thing that would have caught it.
 */
export function familyAreaShares(family, areas, familyCount) {
  const share = new Array(familyCount).fill(0);
  let total = 0;
  for (let i = 0; i < family.length; ++i) { share[family[i]] += areas[i]; total += areas[i]; }
  return share.map(v => v / (total || 1));
}
