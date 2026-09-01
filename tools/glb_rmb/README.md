# GLB -> Room Mesh Bake importer

This is a host-only preprocessing tool for the Game Gear baked renderer. It accepts ordinary, Draco-compressed, or EXT_meshopt_compression GLB files and emits compact signed-Q8 C arrays for the existing `RMBScene` baker.

For the Doomguy chamber:

```bash
cd tools/glb_rmb
npm install
node convert.mjs ../../FullDoomguyclassic-single-notex.glb ../generated/doomguy_mesh.inc \
  --name doomguy --height 19 --up z --visual-tris 1800 --shadow-tris 350
```

The converter deliberately discards normals, UVs, materials, and other attributes before simplification. The GG bake path uses geometry only and computes its own quantized face lighting, so retaining those attributes would prevent useful vertex welding and waste host-side complexity.

Two meshes are generated from the same normalized master:

- **visual**: higher-detail, rendered into the host framebuffer, normally does not cast baked shadows;
- **shadow**: coarse silhouette proxy, normally invisible but participates in light occlusion.

The source master never enters the ROM or Game Gear RAM. Only final baked tile/name-table output does.
