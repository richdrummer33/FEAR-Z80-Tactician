#!/usr/bin/env node
import { Document, NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { meshopt } from '@gltf-transform/functions';
import { MeshoptEncoder } from 'meshoptimizer';

const output=process.argv[2];
if(!output) throw new Error('usage: make_fixture.mjs OUTPUT.glb');

const doc=new Document();
const buffer=doc.createBuffer();
const positions=new Float32Array([
  -1,-1,0,  1,-1,0,  1,1,0, -1,1,0,
  -1,-1,2,  1,-1,2,  1,1,2, -1,1,2
]);
const indices=new Uint16Array([
  0,2,1, 0,3,2, 4,5,6, 4,6,7,
  0,1,5, 0,5,4, 1,2,6, 1,6,5,
  2,3,7, 2,7,6, 3,0,4, 3,4,7
]);
const pos=doc.createAccessor('POSITION').setType('VEC3').setArray(positions).setBuffer(buffer);
const idx=doc.createAccessor('INDICES').setType('SCALAR').setArray(indices).setBuffer(buffer);
const prim=doc.createPrimitive().setAttribute('POSITION',pos).setIndices(idx);
const mesh=doc.createMesh('fixture').addPrimitive(prim);
const scene=doc.createScene('scene').addChild(doc.createNode('fixture-node').setMesh(mesh));
doc.getRoot().setDefaultScene(scene);

await MeshoptEncoder.ready;
await doc.transform(meshopt({encoder:MeshoptEncoder,level:'medium'}));
const io=new NodeIO()
  .registerExtensions(ALL_EXTENSIONS)
  .registerDependencies({'meshopt.encoder':MeshoptEncoder});
await io.write(output,doc);
