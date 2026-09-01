#!/usr/bin/env node
import fs from 'node:fs/promises';
import path from 'node:path';
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { prune, simplify, weld } from '@gltf-transform/functions';
import draco3d from 'draco3dgltf';
import { MeshoptDecoder, MeshoptSimplifier } from 'meshoptimizer';

function fail(msg) { console.error('fatal:', msg); process.exit(2); }
function argValue(args, name, fallback) {
  const i=args.indexOf(name); return i>=0 && i+1<args.length ? args[i+1] : fallback;
}
function sanitizeName(s) {
  const v=s.replace(/[^A-Za-z0-9_]/g,'_').replace(/^[0-9]/,'_$&');
  if(!v) fail('empty --name after sanitizing');
  return v;
}
function mapAxes(p, up) {
  if(up==='z') return [p[0],p[1],p[2]];
  if(up==='y') return [p[0],-p[2],p[1]]; // right-handed Y-up -> Z-up.
  if(up==='x') return [p[1],p[2],p[0]];  // right-handed X-up -> Z-up.
  fail('--up must be x, y, or z');
}
function transformPoint(m,p) {
  return [
    m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],
    m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],
    m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]
  ];
}
function primitiveTriangleCount(prim) {
  const pos=prim.getAttribute('POSITION');
  const idx=prim.getIndices();
  if(!pos) return 0;
  return Math.floor((idx ? idx.getCount() : pos.getCount())/3);
}
function documentTriangleCount(doc) {
  let n=0;
  for(const mesh of doc.getRoot().listMeshes())
    for(const prim of mesh.listPrimitives()) n+=primitiveTriangleCount(prim);
  return n;
}
function dropBakeIrrelevantAttributes(doc) {
  for(const mesh of doc.getRoot().listMeshes()) {
    for(const prim of mesh.listPrimitives()) {
      for(const semantic of prim.listSemantics()) {
        if(semantic!=='POSITION') prim.getAttribute(semantic)?.dispose();
      }
      for(const target of prim.listTargets()) target.dispose();
    }
  }
}
function collectWorldGeometry(doc) {
  const xyz=[], indices=[];
  let base=0;
  const meshNodes=doc.getRoot().listNodes().filter(n=>n.getMesh());
  if(!meshNodes.length) fail('GLB has no mesh-bearing nodes');
  for(const node of meshNodes) {
    const matrix=node.getWorldMatrix();
    for(const prim of node.getMesh().listPrimitives()) {
      if(prim.getMode()!==4) fail('only TRIANGLES primitives are supported');
      const pos=prim.getAttribute('POSITION');
      if(!pos) fail('mesh primitive has no POSITION accessor');
      const arr=pos.getArray();
      for(let i=0;i<pos.getCount();++i) {
        const p=transformPoint(matrix,[arr[i*3],arr[i*3+1],arr[i*3+2]]);
        xyz.push(p[0],p[1],p[2]);
      }
      const idx=prim.getIndices();
      if(idx) {
        const ia=idx.getArray();
        for(let i=0;i<idx.getCount();++i) indices.push(base+Number(ia[i]));
      } else {
        for(let i=0;i<pos.getCount();++i) indices.push(base+i);
      }
      base+=pos.getCount();
    }
  }
  if(indices.length%3) fail('triangle index count is not divisible by three');
  return {xyz,indices,vertexCount:base,triangleCount:indices.length/3};
}
function mappedBounds(geom, up) {
  const mn=[Infinity,Infinity,Infinity], mx=[-Infinity,-Infinity,-Infinity];
  for(let i=0;i<geom.vertexCount;++i) {
    const q=mapAxes([geom.xyz[i*3],geom.xyz[i*3+1],geom.xyz[i*3+2]],up);
    for(let a=0;a<3;++a){ if(q[a]<mn[a])mn[a]=q[a]; if(q[a]>mx[a])mx[a]=q[a]; }
  }
  return {min:mn,max:mx};
}
function normalizeToQ8(geom, up, norm) {
  const out=[];
  for(let i=0;i<geom.vertexCount;++i) {
    const q=mapAxes([geom.xyz[i*3],geom.xyz[i*3+1],geom.xyz[i*3+2]],up);
    const n=[
      (q[0]-norm.cx)*norm.scale,
      (q[1]-norm.cy)*norm.scale,
      (q[2]-norm.z0)*norm.scale
    ];
    for(const v of n) {
      const z=Math.round(v*256);
      if(z<-32768||z>32767) fail('Q8 coordinate overflow; reduce --height');
      out.push(z);
    }
  }
  return out;
}
async function makeIO() {
  await MeshoptDecoder.ready;
  const decoder=await draco3d.createDecoderModule();
  return new NodeIO()
    .registerExtensions(ALL_EXTENSIONS)
    .registerDependencies({'meshopt.decoder':MeshoptDecoder,'draco3d.decoder':decoder});
}
async function simplified(io,input,target) {
  const doc=await io.read(input);
  dropBakeIrrelevantAttributes(doc);
  await doc.transform(weld());
  const before=documentTriangleCount(doc);
  if(before>target) {
    await MeshoptSimplifier.ready;
    await doc.transform(simplify({
      simplifier:MeshoptSimplifier,
      ratio:Math.max(0.000001,Math.min(1,target/before)),
      error:1,
      lockBorder:false
    }), prune());
  } else {
    await doc.transform(prune());
  }
  return doc;
}
function cArray(type,name,values,perLine=12) {
  let s=`static const ${type} ${name}[] = {\n`;
  for(let i=0;i<values.length;i+=perLine)
    s+='    '+values.slice(i,i+perLine).join(', ') + (i+perLine<values.length?',':'') + '\n';
  return s+'};\n';
}
function emitMesh(prefix,kind,geom,q8) {
  const base=`${prefix}_${kind}`;
  if(geom.vertexCount>65535) fail(`${kind} mesh exceeds uint16 indexable vertex count`);
  return [
    `#define ${base.toUpperCase()}_VERTEX_COUNT ${geom.vertexCount}u\n`,
    `#define ${base.toUpperCase()}_TRIANGLE_COUNT ${geom.triangleCount}u\n`,
    cArray('int16_t',base+'_xyz_q8',q8,12),
    cArray('uint16_t',base+'_indices',geom.indices,12)
  ].join('');
}

const args=process.argv.slice(2);
if(args.length<2) fail('usage: convert.mjs INPUT.glb OUTPUT.inc [--name doomguy] [--height 19] [--up z] [--visual-tris 1800] [--lighting-tris 72] [--shadow-tris 350]');
const input=args[0], output=args[1];
const name=sanitizeName(argValue(args,'--name','doomguy'));
const height=Number(argValue(args,'--height','19'));
const up=String(argValue(args,'--up','z')).toLowerCase();
const visualTarget=Math.max(4,Number(argValue(args,'--visual-tris','1800'))|0);
const lightingTarget=Math.max(4,Number(argValue(args,'--lighting-tris','72'))|0);
const shadowTarget=Math.max(4,Number(argValue(args,'--shadow-tris','350'))|0);
if(!(height>0)) fail('--height must be positive');

const io=await makeIO();
const source=await io.read(input);
const sourceGeom=collectWorldGeometry(source);
const b=mappedBounds(sourceGeom,up);
const rawHeight=b.max[2]-b.min[2];
if(!(rawHeight>1e-12)) fail('selected up axis has zero height');
const norm={cx:(b.min[0]+b.max[0])/2,cy:(b.min[1]+b.max[1])/2,z0:b.min[2],scale:height/rawHeight};

const visualDoc=await simplified(io,input,visualTarget);
const lightingDoc=await simplified(io,input,lightingTarget);
const shadowDoc=await simplified(io,input,shadowTarget);
const visualGeom=collectWorldGeometry(visualDoc);
const lightingGeom=collectWorldGeometry(lightingDoc);
const shadowGeom=collectWorldGeometry(shadowDoc);
const visualQ8=normalizeToQ8(visualGeom,up,norm);
const lightingQ8=normalizeToQ8(lightingGeom,up,norm);
const shadowQ8=normalizeToQ8(shadowGeom,up,norm);

const header=`/* Generated by tools/glb_rmb/convert.mjs. DO NOT HAND EDIT.
 * source: ${path.basename(input)}
 * source vertices/triangles: ${sourceGeom.vertexCount}/${sourceGeom.triangleCount}
 * source mapped bounds: [${b.min.map(v=>v.toFixed(6)).join(', ')}] .. [${b.max.map(v=>v.toFixed(6)).join(', ')}]
 * normalization: up=${up}, target_height=${height}, scale=${norm.scale}
 * visual vertices/triangles: ${visualGeom.vertexCount}/${visualGeom.triangleCount}
 * lighting vertices/triangles: ${lightingGeom.vertexCount}/${lightingGeom.triangleCount}
 * shadow vertices/triangles: ${shadowGeom.vertexCount}/${shadowGeom.triangleCount}
 */
`;
const body=header+
  emitMesh(name,'visual',visualGeom,visualQ8)+'\n'+
  emitMesh(name,'lighting',lightingGeom,lightingQ8)+'\n'+
  emitMesh(name,'shadow',shadowGeom,shadowQ8);
await fs.mkdir(path.dirname(output),{recursive:true});
await fs.writeFile(output,body,'utf8');

const stats={
  input:path.resolve(input), output:path.resolve(output), name, up, targetHeight:height,
  source:{vertices:sourceGeom.vertexCount,triangles:sourceGeom.triangleCount,bounds:b},
  visual:{vertices:visualGeom.vertexCount,triangles:visualGeom.triangleCount,target:visualTarget},
  lighting:{vertices:lightingGeom.vertexCount,triangles:lightingGeom.triangleCount,target:lightingTarget},
  shadow:{vertices:shadowGeom.vertexCount,triangles:shadowGeom.triangleCount,target:shadowTarget},
  q8:{scale:norm.scale,centerXY:[norm.cx,norm.cy],baseZ:norm.z0}
};
console.log(JSON.stringify(stats,null,2));
