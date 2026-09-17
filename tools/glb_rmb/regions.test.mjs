/*
 * Guard tests for authored material regions.
 *
 * Run: node tools/glb_rmb/regions.test.mjs
 *
 * A region file is the one place in this pipeline where a human states
 * something about an asset instead of the pipeline measuring it, so the thing
 * worth testing is not the arithmetic of a sphere test -- it is everything
 * that makes an authored statement fail QUIETLY: a coordinate typo that
 * selects nothing, an overlap rule that is ambiguous, a family that ends up
 * empty and silently costs five palette entries.
 */
import assert from 'node:assert/strict';
import { loadRegionSpec, assignFamilies, familyAreaShares } from './regions.mjs';

let failures = 0;
function test(name, fn) {
  try { fn(); console.log(`  ok  ${name}`); }
  catch (e) { failures++; console.log(`FAIL  ${name}\n      ${e.message}`); }
}

const TWO_FAMILIES = [
  { name: 'body', srgb: [204, 85, 68] },
  { name: 'kit', srgb: [150, 128, 118], lightnessOffset: -0.12 }
];

function spec(regions, extra = {}) {
  return loadRegionSpec({ families: TWO_FAMILIES, regions, ...extra });
}
function pts(...ps) {
  const a = new Float64Array(ps.length * 3);
  ps.forEach((p, i) => { a[i * 3] = p[0]; a[i * 3 + 1] = p[1]; a[i * 3 + 2] = p[2]; });
  return a;
}

test('a halfspace selects exactly its slab, boundaries included', () => {
  const s = spec([{ family: 1, type: 'halfspace', axis: 'z', min: 2, max: 5 }]);
  const p = pts([0, 0, 1.9], [0, 0, 2], [0, 0, 3.5], [0, 0, 5], [0, 0, 5.1]);
  const { family } = assignFamilies(s, p, 5);
  assert.deepEqual([...family], [0, 1, 1, 1, 0]);
});

test('a halfspace with one bound open reaches to infinity on that side', () => {
  const s = spec([{ family: 1, type: 'halfspace', axis: 'y', max: 0 }]);
  const { family } = assignFamilies(s, pts([0, -1e6, 0], [0, 0.1, 0]), 2);
  assert.deepEqual([...family], [1, 0]);
});

test('a box is inclusive on all six faces', () => {
  const s = spec([{ family: 1, type: 'box', min: [-1, -1, -1], max: [1, 1, 1] }]);
  const { family } = assignFamilies(
    s, pts([1, 1, 1], [-1, -1, -1], [0, 0, 0], [1.0001, 0, 0]), 4);
  assert.deepEqual([...family], [1, 1, 1, 0]);
});

test('a sphere measures true distance, not a bounding box', () => {
  const s = spec([{ family: 1, type: 'sphere', center: [0, 0, 0], radius: 1 }]);
  /* The cube corner at (0.6,0.6,0.6) is inside the sphere's AABB and outside
   * the sphere itself; a bounding-box implementation passes the other three. */
  const { family } = assignFamilies(
    s, pts([0.99, 0, 0], [0, 0, 1], [0.6, 0.6, 0.6], [0.5, 0.5, 0.5]), 4);
  assert.deepEqual([...family], [1, 1, 0, 1]);
});

test('a capsule follows its segment and caps at the ends', () => {
  const s = spec([{ family: 1, type: 'capsule', a: [0, 0, 0], b: [10, 0, 0], radius: 1 }]);
  const { family } = assignFamilies(s, pts(
    [5, 0.9, 0],     /* beside the middle, inside       */
    [5, 1.1, 0],     /* beside the middle, outside      */
    [-0.9, 0, 0],    /* inside the round cap past a     */
    [-1.1, 0, 0],    /* past the cap                    */
    [10.5, 0.5, 0]   /* inside the round cap past b     */
  ), 5);
  assert.deepEqual([...family], [1, 0, 1, 0, 1]);
});

test('a capsule is not its bounding box: the diagonal case', () => {
  /* The whole reason capsules exist here is the gun, which runs diagonally.
   * A point at the corner of the segment's AABB is far from the segment. */
  const s = spec([{ family: 1, type: 'capsule', a: [0, 0, 0], b: [4, -4, 0], radius: 1 }]);
  const { family } = assignFamilies(s, pts([2, -2, 0], [0, -4, 0], [4, 0, 0]), 3);
  assert.deepEqual([...family], [1, 0, 0]);
});

test('later regions win, which is what makes subtraction free', () => {
  const s = spec([
    { family: 1, type: 'halfspace', axis: 'z', min: 0, max: 10 },
    { family: 0, type: 'sphere', center: [0, 0, 5], radius: 1 }
  ]);
  const { family } = assignFamilies(s, pts([0, 0, 5], [0, 0, 8]), 2);
  assert.deepEqual([...family], [0, 1]);
});

test('the overlap rule is order, not index: swapping the two swaps the answer', () => {
  const s = spec([
    { family: 0, type: 'sphere', center: [0, 0, 5], radius: 1 },
    { family: 1, type: 'halfspace', axis: 'z', min: 0, max: 10 }
  ]);
  const { family } = assignFamilies(s, pts([0, 0, 5]), 1);
  assert.deepEqual([...family], [1]);
});

test('hit counts report per region, so an empty one can be caught', () => {
  const s = spec([
    { family: 1, type: 'sphere', center: [0, 0, 0], radius: 1 },
    { family: 1, type: 'sphere', center: [100, 0, 0], radius: 1 }
  ]);
  const { hits } = assignFamilies(s, pts([0, 0, 0], [0.5, 0, 0]), 2);
  assert.deepEqual(hits, [2, 0]);
});

test('area shares are area weighted, not vertex counts', () => {
  const family = Uint8Array.from([0, 1, 1]);
  /* Two small kit vertices against one large body vertex: counting vertices
   * would say the kit is 2/3 of the model and it is a fifth of it. */
  const share = familyAreaShares(family, [8, 1, 1], 2);
  assert.ok(Math.abs(share[0] - 0.8) < 1e-9, `body share ${share[0]}`);
  assert.ok(Math.abs(share[1] - 0.2) < 1e-9, `kit share ${share[1]}`);
});

test('the default family fills everything no region claims', () => {
  const s = spec([{ family: 1, type: 'sphere', center: [0, 0, 0], radius: 1 }],
                 { default: 0 });
  const { family } = assignFamilies(s, pts([50, 50, 50]), 1);
  assert.equal(family[0], 0);
});

test('a non-zero default is honoured', () => {
  const s = spec([{ family: 0, type: 'sphere', center: [0, 0, 0], radius: 1 }],
                 { default: 1 });
  const { family } = assignFamilies(s, pts([50, 0, 0], [0, 0, 0]), 2);
  assert.deepEqual([...family], [1, 0]);
});

/* ---- everything below is a statement that should be REFUSED --------------
 * Each of these produces a plausible-looking build if it is accepted. */

function rejects(what, obj) {
  test(`rejects ${what}`, () => {
    assert.throws(() => loadRegionSpec(obj), /region spec/);
  });
}

rejects('a spec with no families', { regions: [{ family: 0, type: 'box', min: [0, 0, 0], max: [1, 1, 1] }] });
rejects('a spec with no regions', { families: TWO_FAMILIES, regions: [] });
rejects('a family index past the end of the family list',
  { families: TWO_FAMILIES, regions: [{ family: 2, type: 'sphere', center: [0, 0, 0], radius: 1 }] });
rejects('a default family past the end of the family list',
  { families: TWO_FAMILIES, default: 5, regions: [{ family: 0, type: 'sphere', center: [0, 0, 0], radius: 1 }] });
rejects('an unknown region type',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'torus', center: [0, 0, 0], radius: 1 }] });
rejects('a sphere with no radius',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'sphere', center: [0, 0, 0] }] });
rejects('a negative radius',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'sphere', center: [0, 0, 0], radius: -1 }] });
rejects('a capsule whose endpoints coincide',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'capsule', a: [0, 0, 0], b: [0, 0, 0], radius: 1 }] });
rejects('an inverted box',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'box', min: [1, 0, 0], max: [0, 1, 1] }] });
rejects('a halfspace that bounds nothing',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'halfspace', axis: 'z' }] });
rejects('a halfspace on a made-up axis',
  { families: TWO_FAMILIES, regions: [{ family: 1, type: 'halfspace', axis: 'w', min: 0, max: 1 }] });
rejects('an out-of-range srgb channel',
  { families: [{ name: 'a', srgb: [300, 0, 0] }], regions: [{ family: 0, type: 'sphere', center: [0, 0, 0], radius: 1 }] });
rejects('more families than the sprite palette has entries',
  { families: Array.from({ length: 16 }, (_, i) => ({ name: `f${i}`, srgb: [128, 128, 128] })),
    regions: [{ family: 0, type: 'sphere', center: [0, 0, 0], radius: 1 }] });

test('a vertex is never assigned a family outside the declared list', () => {
  const s = spec([
    { family: 1, type: 'capsule', a: [0, 0, 0], b: [1, 1, 1], radius: 2 },
    { family: 0, type: 'box', min: [-9, -9, -9], max: [9, 9, 9] },
    { family: 1, type: 'sphere', center: [3, 3, 3], radius: 4 }
  ]);
  const n = 400;
  const p = new Float64Array(n * 3);
  for (let i = 0; i < n * 3; ++i) p[i] = ((i * 2654435761) % 2001) / 100 - 10;
  const { family } = assignFamilies(s, p, n);
  for (const f of family) assert.ok(f === 0 || f === 1, `family ${f} is not declared`);
});

console.log(failures ? `\n${failures} region test(s) failed` : '\nall region tests passed');
process.exit(failures ? 1 : 0);
