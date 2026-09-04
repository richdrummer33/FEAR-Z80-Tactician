# Doomguy / Hunyuan Diorama Material-Form Tri-Run — 2026-09-04

## Basis

- Parent: `temp/hero-lod-shape-objective` at `da8f987574413568e812e8d18296e00b4c51095f`.
- Experiment branch: `temp/doomguy-material-form-latest`.
- External test asset SHA-256: `d17bbeb8b2a1b38e22b3368573c5dc72212a0a36c77df8f37174a094edd63af0`.
- Source asset: 185365 vertices / 248608 triangles, base-colour texture, normal map and metallic/roughness texture; no dedicated occlusion map.
- Asset is Y-up after its node transform. Final reviewed import used `--up y --height 12.2 --visual-tris 5200 --shadow-tris 200 --recess-radius 0.5`.
- Final shell: 2560 vertices / 5194 triangles; shadow proxy: 90 vertices / 188 triangles.

The old 19-unit hero normalization overhung the centre plinth because this asset is a broad diorama rather than the previous single figure. A 12.2-unit import height, followed by the room's existing 1.35 art-direction scale, produces an approximately 17.94 x 15.04 x 16.47 world-unit piece on the 20 x 18 plinth. Bounds-centering plus min-Z anchoring makes placement independent of the source pivot.

## Importer experiment

`tools/glb_rmb/convert.mjs` gains `--shading-source geometry|hybrid|material`, `--material-strength`, and `--material-blur`.

For assets without a dedicated AO map, material form is extracted primarily from locally-dark base-colour residuals with normal-map high-frequency detail as a secondary cue. Global dark albedo is deliberately not treated as shadow. The hybrid field is combined with geometric recess in complement space so both can deepen a fold without simple additive clipping.

Three matched variants were baked:

1. `geometry`: existing geometric recess + current static lighting/AO/crease/equalized five-stop ramp.
2. `hybrid`: geometric recess plus material-derived form, using the same renderer settings.
3. `material`: material-form isolation control; statue incident/AO/shadow terms are neutralized so the imported form field drives its tonal structure, while the same five-stop quantizer/dither remains in use.

## Room-bake transport

All variants passed canonical replay `E4F108D3C424CCE3`.

| Variant | Route | Tile loads | Tile bytes | Scheduled peak | Changed words |
| --- | --- | ---: | ---: | ---: | ---: |
| geometry | 0->1 | 5340 | 182328 | 55 | 15395 |
| geometry | 1->0 | 5322 | 181716 | 56 | 15395 |
| hybrid | 0->1 | 5340 | 182328 | 55 | 15394 |
| hybrid | 1->0 | 5322 | 181716 | 56 | 15394 |
| material | 0->1 | 5319 | 181614 | 55 | 15327 |
| material | 1->0 | 5305 | 181138 | 56 | 15327 |

The material cue therefore adds essentially no transport cost in the hybrid run.

## Limited-traversal playable pack

| Variant | Positions | States | Mean patterns | Peak | Avg VBlanks | Peak VBlanks | Model avg Hz | Model worst Hz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| geometry | 40 | 640 | 68.12 | 138 | 3.93 | 5 | 15.26 | 12.00 |
| hybrid | 40 | 640 | 68.12 | 138 | 3.93 | 5 | 15.26 | 12.00 |
| material | 40 | 640 | 68.12 | 139 | 3.93 | 5 | 15.26 | 12.00 |

These Hz figures are the existing transport/headroom model, not measured physical Game Gear FPS.

For comparison, the successful 2026-09-02 Doomguy playable artifact at `90c7f285...` had 38 positions / 608 states, mean 70.21 patterns, peak 145, average 3.96 VBlanks, peak 6, theoretical average 15.14 Hz and theoretical worst transport 10 Hz. The new geometry/hybrid pack therefore has two more legal camera positions, about 3% lower mean pattern demand, about 4.8% lower worst pattern demand, and a better worst publication tail.

## Corrected brightness-order LOD objective

At far radius 36, eight test angles, with the deliberately-starved 16-pattern dictionary:

| Variant | Raw-enum >=3-stop errors | Perceptual >=3-stop errors | Reduction | Perceptual mean stops wrong |
| --- | ---: | ---: | ---: | ---: |
| geometry | 1221 | 110 | 11.1x | 0.8306 |
| hybrid | 1228 | 139 | 8.8x | 0.8556 |
| material | 1126 | 104 | 10.8x | 0.8568 |

This reproduces the latest branch's diagnosis on the new asset: brightness-order distance fixes gross tonal fitting mistakes while silhouette remains constrained by the tiny vocabulary.

At the same far band with 128 patterns and the perceptual objective:

| Variant | Silhouette error | >=3-stop errors | Mean stops wrong |
| --- | ---: | ---: | ---: |
| geometry | 1.66% | 26 | 0.1530 |
| hybrid | 1.66% | 25 | 0.1523 |
| material | 1.81% | 30 | 0.1693 |

The 128-pattern result is dramatically stronger than the 16-pattern core. Hybrid is tied on silhouette and narrowly best on mean tonal error in this test.

## Visual conclusion

Geometry remains the strongest broad light-driven form baseline. Hybrid is intentionally subtle at the current conservative defaults, changing only about 0.2–0.3% of sampled full-screen pixels relative to geometry, but it retains the geometric form while adding material cues at effectively zero extra room transport cost. Material-only changes roughly 6–8.5% of sampled pixels and preserves texture-authored internal structure, but loses some coherent broad curvature/light turning.

Current preferred direction: **hybrid**. A later tuning sweep can deliberately increase material influence if a more obvious visible contribution is desired without changing the basic architecture.
