#!/usr/bin/env node
import fs from 'node:fs/promises';
import path from 'node:path';
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { prune, simplify, weld } from '@gltf-transform/functions';
import draco3d from 'draco3dgltf';
import { MeshoptDecoder, MeshoptSimplifier } from 'meshoptimizer';
import sharp from 'sharp';
import { computeSourceConcavity, transferToShell } from './recess.mjs';
import { srgbToLinear, linearToOklab, kmeansOklab, synthesizeRamp,
         solveRamp, oklabDistance, ggToSrgb8, transferVec3ToShell,
         materialResidual, chromaGridStep } from './palette.mjs';

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
function normalizedPositions(geom,up,norm) {
  const out=new Float64Array(geom.vertexCount*3);
  for(let i=0;i<geom.vertexCount;++i){
    const q=mapAxes([geom.xyz[i*3],geom.xyz[i*3+1],geom.xyz[i*3+2]],up);
    out[i*3]=(q[0]-norm.cx)*norm.scale;
    out[i*3+1]=(q[1]-norm.cy)*norm.scale;
    out[i*3+2]=(q[2]-norm.z0)*norm.scale;
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
function emitRecess(prefix,kind,values) {
  const base=`${prefix}_${kind}`;
  return cArray('uint8_t',base+'_recess',Array.from(values),16);
}
function emitHue(prefix,kind,values) {
  return cArray('uint8_t',`${prefix}_${kind}_hue`,Array.from(values),24);
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
function percentile(values,p) {
  if(!values.length) return 0;
  const s=Array.from(values).sort((a,b)=>a-b);
  return s[Math.min(s.length-1,Math.max(0,Math.round(p*(s.length-1))))];
}
function clamp01(v){ return v<0?0:(v>1?1:v); }
function wrap01(v){ v-=Math.floor(v); return v<0?v+1:v; }
function sampleBilinear(field,u,v) {
  if(!field) return 0;
  u=wrap01(u); v=wrap01(v);
  const x=u*(field.width-1), y=v*(field.height-1);
  const x0=Math.floor(x), y0=Math.floor(y);
  const x1=Math.min(field.width-1,x0+1), y1=Math.min(field.height-1,y0+1);
  const fx=x-x0, fy=y-y0;
  const at=(xx,yy)=>field.data[yy*field.width+xx];
  const a=at(x0,y0)*(1-fx)+at(x1,y0)*fx;
  const b=at(x0,y1)*(1-fx)+at(x1,y1)*fx;
  return a*(1-fy)+b*fy;
}
function luma8(data,i,channels){
  return 0.2126*data[i*channels]+0.7152*data[i*channels+1]+0.0722*data[i*channels+2];
}
async function decodeTextureFeatures(texture,kind,blurSigma) {
  if(!texture?.getImage()) return null;
  const input=Buffer.from(texture.getImage());
  const base=sharp(input,{failOn:'none'}).removeAlpha();
  const raw=await base.clone().raw().toBuffer({resolveWithObject:true});
  const w=raw.info.width,h=raw.info.height,c=raw.info.channels;
  const out=new Float32Array(w*h);
  if(kind==='occlusion'){
    for(let i=0;i<w*h;++i) out[i]=1-raw.data[i*c]/255;
    return {data:out,width:w,height:h};
  }
  const blur1=await base.clone().blur(Math.max(0.3,blurSigma)).raw().toBuffer({resolveWithObject:true});
  if(kind==='normal'){
    for(let i=0;i<w*h;++i){
      const o=i*c;
      const dr=(raw.data[o]-blur1.data[o])/255;
      const dg=(raw.data[o+1]-blur1.data[o+1])/255;
      const db=(raw.data[o+2]-blur1.data[o+2])/255;
      out[i]=clamp01(Math.sqrt(dr*dr+dg*dg+db*db)*2.0);
    }
    return {data:out,width:w,height:h};
  }
  const blur2=await base.clone().blur(Math.max(0.3,blurSigma*4)).raw().toBuffer({resolveWithObject:true});
  for(let i=0;i<w*h;++i){
    const y0=luma8(raw.data,i,c);
    const y1=luma8(blur1.data,i,c);
    const y2=luma8(blur2.data,i,c);
    const local=Math.max(0,y1-y0)/255;
    const broad=Math.max(0,y2-y0)/255;
    out[i]=clamp01(Math.max(local,broad*0.65)*3.0);
  }
  return {data:out,width:w,height:h};
}
async function buildMaterialSampler(material,blurSigma,cache,meta){
  if(!material) return null;
  if(cache.has(material)) return cache.get(material);
  const baseTex=material.getBaseColorTexture?.() || null;
  const normalTex=material.getNormalTexture?.() || null;
  const occTex=material.getOcclusionTexture?.() || null;
  const baseInfo=material.getBaseColorTextureInfo?.() || null;
  const normalInfo=material.getNormalTextureInfo?.() || null;
  const occInfo=material.getOcclusionTextureInfo?.() || null;
  for(const [label,info] of [['baseColor',baseInfo],['normal',normalInfo],['occlusion',occInfo]]){
    if(info && typeof info.getTexCoord==='function' && info.getTexCoord()!==0)
      fail(`${label} texture uses TEXCOORD_${info.getTexCoord()}; material-form currently supports TEXCOORD_0`);
  }
  const [base,normal,occ]=await Promise.all([
    decodeTextureFeatures(baseTex,'base',blurSigma),
    decodeTextureFeatures(normalTex,'normal',Math.max(1,blurSigma*0.5)),
    decodeTextureFeatures(occTex,'occlusion',blurSigma)
  ]);
  if(base)meta.baseColor=true;
  if(normal)meta.normal=true;
  if(occ)meta.occlusion=true;
  const sampler=(u,v)=>{
    const b=sampleBilinear(base,u,v);
    const n=sampleBilinear(normal,u,v);
    const o=sampleBilinear(occ,u,v);
    /* Explicit AO wins when present. Otherwise locally-dark colour residuals
     * carry most of the signal, with normal-map high-frequency detail as a
     * secondary crease cue. This deliberately avoids treating black armour as
     * a shadow merely because its albedo is black. */
    return occ ? clamp01(0.55*o+0.30*b+0.15*n)
               : clamp01(0.75*b+0.25*n);
  };
  cache.set(material,sampler);
  return sampler;
}
/*
 * Base-colour (albedo) extraction, kept separate from the scalar "material
 * form" field above on purpose. That field asks "is this pixel in a crease",
 * and to answer it correctly it must NOT treat black armour as a shadow. This
 * one asks the opposite question -- "what colour is this material" -- so it
 * wants exactly the albedo the other one is trying to discount.
 */
async function decodeBaseColorRGB(texture) {
  if(!texture?.getImage()) return null;
  const raw=await sharp(Buffer.from(texture.getImage()),{failOn:'none'})
    .removeAlpha().raw().toBuffer({resolveWithObject:true});
  const w=raw.info.width,h=raw.info.height,c=raw.info.channels;
  const out=new Float32Array(w*h*3);
  for(let i=0;i<w*h;++i)
    for(let k=0;k<3;++k) out[i*3+k]=srgbToLinear(raw.data[i*c+k]/255);
  return {data:out,width:w,height:h};
}
function sampleBilinearRGB(field,u,v) {
  if(!field) return null;
  u=u-Math.floor(u); v=v-Math.floor(v);
  const x=u*(field.width-1), y=v*(field.height-1);
  const x0=Math.floor(x), y0=Math.floor(y);
  const x1=Math.min(field.width-1,x0+1), y1=Math.min(field.height-1,y0+1);
  const fx=x-x0, fy=y-y0;
  const out=[0,0,0];
  for(let k=0;k<3;++k){
    const at=(xx,yy)=>field.data[(yy*field.width+xx)*3+k];
    const a=at(x0,y0)*(1-fx)+at(x1,y0)*fx;
    const b=at(x0,y1)*(1-fx)+at(x1,y1)*fx;
    out[k]=a*(1-fy)+b*fy;
  }
  return out;
}
async function buildAlbedoSampler(material,cache,meta) {
  if(!material) return null;
  if(cache.has(material)) return cache.get(material);
  const tex=await decodeBaseColorRGB(material.getBaseColorTexture?.()||null);
  /* glTF baseColorFactor is already linear and multiplies the texture. A
   * material with a factor and no texture is a perfectly ordinary flat
   * material and must still contribute a family. */
  const f=material.getBaseColorFactor?.()||[1,1,1,1];
  if(tex)meta.albedoTexture=true; else meta.albedoFactorOnly++;
  const sampler=(uv)=>{
    const t=uv?sampleBilinearRGB(tex,uv[0],uv[1]):null;
    return t? [t[0]*f[0],t[1]*f[1],t[2]*f[2]] : [f[0],f[1],f[2]];
  };
  cache.set(material,sampler);
  return sampler;
}
async function collectWorldAlbedo(doc) {
  const xyz=[], rgb=[];
  const cache=new Map();
  const meta={albedoTexture:false,albedoFactorOnly:0,untexturedPrimitives:0};
  let count=0;
  for(const node of doc.getRoot().listNodes().filter(n=>n.getMesh())){
    const matrix=node.getWorldMatrix();
    for(const prim of node.getMesh().listPrimitives()){
      const pos=prim.getAttribute('POSITION');
      if(!pos)continue;
      const uv=prim.getAttribute('TEXCOORD_0');
      const pa=pos.getArray(), ua=uv?.getArray();
      const sampler=await buildAlbedoSampler(prim.getMaterial?.(),cache,meta);
      if(!ua)meta.untexturedPrimitives++;
      for(let i=0;i<pos.getCount();++i){
        const p=transformPoint(matrix,[pa[i*3],pa[i*3+1],pa[i*3+2]]);
        const c=sampler?sampler(ua?[ua[i*2],ua[i*2+1]]:null):[0.5,0.5,0.5];
        xyz.push(p[0],p[1],p[2]);
        rgb.push(c[0],c[1],c[2]);
        count++;
      }
    }
  }
  if(!count)fail('no vertices carried base colour; cannot derive a palette');
  meta.vertices=count;
  return {xyz,rgb:Float32Array.from(rgb),vertexCount:count,meta};
}
/*
 * Per-vertex surface area on the shell: one third of each incident triangle.
 * The family clustering is weighted by this and not by vertex count, because
 * decimation does not distribute vertices by visual importance -- it leaves a
 * broad flat panel with a handful of vertices and a filigreed edge with
 * hundreds. Counting vertices would let a detail nobody can see at 32 units
 * outvote the material that covers half the silhouette.
 */
function vertexAreas(geom,q8) {
  const area=new Float64Array(geom.vertexCount);
  for(let t=0;t<geom.triangleCount;++t){
    const i=geom.indices[t*3],j=geom.indices[t*3+1],k=geom.indices[t*3+2];
    const ax=q8[i*3]/256,ay=q8[i*3+1]/256,az=q8[i*3+2]/256;
    const bx=q8[j*3]/256,by=q8[j*3+1]/256,bz=q8[j*3+2]/256;
    const cx=q8[k*3]/256,cy=q8[k*3+1]/256,cz=q8[k*3+2]/256;
    const e1=[bx-ax,by-ay,bz-az], e2=[cx-ax,cy-ay,cz-az];
    const nx=e1[1]*e2[2]-e1[2]*e2[1];
    const ny=e1[2]*e2[0]-e1[0]*e2[2];
    const nz=e1[0]*e2[1]-e1[1]*e2[0];
    const a=Math.hypot(nx,ny,nz)/2/3;
    area[i]+=a; area[j]+=a; area[k]+=a;
  }
  return area;
}

async function collectWorldMaterialForm(doc,blurSigma){
  const xyz=[], form=[];
  const cache=new Map();
  const meta={baseColor:false,normal:false,occlusion:false,missingUVPrimitives:0};
  let count=0;
  const meshNodes=doc.getRoot().listNodes().filter(n=>n.getMesh());
  for(const node of meshNodes){
    const matrix=node.getWorldMatrix();
    for(const prim of node.getMesh().listPrimitives()){
      const pos=prim.getAttribute('POSITION');
      const uv=prim.getAttribute('TEXCOORD_0');
      if(!pos)continue;
      const pa=pos.getArray(), ua=uv?.getArray();
      const sampler=await buildMaterialSampler(prim.getMaterial?.(),blurSigma,cache,meta);
      if(!ua)meta.missingUVPrimitives++;
      for(let i=0;i<pos.getCount();++i){
        const p=transformPoint(matrix,[pa[i*3],pa[i*3+1],pa[i*3+2]]);
        xyz.push(p[0],p[1],p[2]);
        form.push(sampler&&ua ? sampler(ua[i*2],ua[i*2+1]) : 0);
        count++;
      }
    }
  }
  const p97=percentile(form,0.97)||1;
  const normField=new Float64Array(count);
  for(let i=0;i<count;++i)normField[i]=clamp01(form[i]/p97);
  meta.vertices=count;
  meta.rawP50=percentile(form,0.50);
  meta.rawP90=percentile(form,0.90);
  meta.rawP97=p97;
  meta.rawMax=Math.max(...form);
  return {xyz,field:normField,vertexCount:count,meta};
}

const args=process.argv.slice(2);
if(args.length<2) fail('usage: convert.mjs INPUT.glb OUTPUT.inc [--name doomguy] [--height 19] [--up z] [--visual-tris 1800] [--lighting-tris 72] [--shadow-tris 350] [--shading-source geometry|hybrid|material] [--material-strength 0.65] [--material-blur 6] [--hue-families 0|4] [--hue-shades 4]');
const input=args[0], output=args[1];
const name=sanitizeName(argValue(args,'--name','doomguy'));
const height=Number(argValue(args,'--height','19'));
const up=String(argValue(args,'--up','z')).toLowerCase();
const visualTarget=Math.max(4,Number(argValue(args,'--visual-tris','1800'))|0);
const lightingTarget=Math.max(4,Number(argValue(args,'--lighting-tris','72'))|0);
const shadowTarget=Math.max(4,Number(argValue(args,'--shadow-tris','350'))|0);
const shadingSource=String(argValue(args,'--shading-source','geometry')).toLowerCase();
const materialStrength=clamp01(Number(argValue(args,'--material-strength','0.65')));
const materialBlur=Math.max(0.5,Number(argValue(args,'--material-blur','6')));
/* World-space radius, in normalized output units, over which source curvature
 * is averaged onto a shell vertex. Match it to the shell's triangle size: a
 * crease cannot be drawn narrower than the shell can represent. */
const recessRadius=Number(argValue(args,'--recess-radius','0.5'));
/* 0 disables colour entirely and the emitted header is byte-identical to a
 * pre-colour build. The interesting value is 4: the runtime palette index is
 * (family<<2)|shade, so anything above 4 does not fit the sprite palette and
 * anything below wastes a bitplane. */
const hueFamilies=Math.max(0,Number(argValue(args,'--hue-families','0'))|0);
const hueShades=Math.max(2,Number(argValue(args,'--hue-shades','4'))|0);
/* The mono-ramp layout reuses the compositor's existing five-stop brightness
 * ramp verbatim, so these two are dictated by polar_baked_composite.c's
 * k_shade_ramp and must not be tuned independently of it. */
const MONO_RAMP_STOPS=5;
const MONO_RAMP_PALETTE_INDICES=[3,6,4,7,5];
if(!(height>0)) fail('--height must be positive');
if(!['geometry','hybrid','material'].includes(shadingSource))
  fail('--shading-source must be geometry, hybrid, or material');
if(hueFamilies>0&&hueFamilies*hueShades>16)
  fail('--hue-families x --hue-shades must fit the 16-entry sprite palette');

const io=await makeIO();
const source=await io.read(input);
const sourceGeom=collectWorldGeometry(source);
const b=mappedBounds(sourceGeom,up);
const rawHeight=b.max[2]-b.min[2];
if(!(rawHeight>1e-12)) fail('selected up axis has zero height');
const norm={cx:(b.min[0]+b.max[0])/2,cy:(b.min[1]+b.max[1])/2,z0:b.min[2],scale:height/rawHeight};

/* Extract texture/material form BEFORE the simplifier discards UVs. This is
 * host-only CV work: it never becomes a runtime texture lookup. */
const sourceMaterial=shadingSource==='geometry' ? null
  : await collectWorldMaterialForm(source,materialBlur);

const visualDoc=await simplified(io,input,visualTarget);
const lightingDoc=await simplified(io,input,lightingTarget);
const shadowDoc=await simplified(io,input,shadowTarget);
const visualGeom=collectWorldGeometry(visualDoc);
const lightingGeom=collectWorldGeometry(lightingDoc);
const shadowGeom=collectWorldGeometry(shadowDoc);
const visualQ8=normalizeToQ8(visualGeom,up,norm);
const lightingQ8=normalizeToQ8(lightingGeom,up,norm);
const shadowQ8=normalizeToQ8(shadowGeom,up,norm);

/* Crease field: full-resolution welded geometry -> decimated visual shell. */
const srcWelded=await (async ()=>{
  const doc=await io.read(input);
  dropBakeIrrelevantAttributes(doc);
  await doc.transform(weld());
  return collectWorldGeometry(doc);
})();
const srcQ8Pos=normalizedPositions(srcWelded,up,norm);
const srcConc=computeSourceConcavity(srcQ8Pos,srcWelded.indices,
                                     srcWelded.vertexCount,srcWelded.triangleCount);
const visualPos=new Float64Array(visualGeom.vertexCount*3);
for(let i=0;i<visualGeom.vertexCount;++i)
  for(let a=0;a<3;++a)visualPos[i*3+a]=visualQ8[i*3+a]/256;
const visualConc=transferToShell(srcQ8Pos,srcConc,srcWelded.vertexCount,
                                 visualPos,visualGeom.vertexCount,recessRadius);
const concMax=visualConc.reduce((m,v)=>v>m?v:m,0)||1;
const geomField=new Float64Array(visualGeom.vertexCount);
for(let i=0;i<visualGeom.vertexCount;++i)
  geomField[i]=visualConc[i]>0?clamp01(visualConc[i]/concMax):0;

let visualMaterial=new Float64Array(visualGeom.vertexCount);
if(sourceMaterial){
  const srcMatPos={xyz:sourceMaterial.xyz,vertexCount:sourceMaterial.vertexCount};
  const srcMatNorm=normalizedPositions(srcMatPos,up,norm);
  visualMaterial=transferToShell(srcMatNorm,sourceMaterial.field,sourceMaterial.vertexCount,
                                 visualPos,visualGeom.vertexCount,recessRadius);
  const p97=percentile(visualMaterial,0.97)||1;
  for(let i=0;i<visualMaterial.length;++i)visualMaterial[i]=clamp01(visualMaterial[i]/p97);
}

/* ---- Material palette ---------------------------------------------------
 * Runs on the SOURCE mesh's texture and is transferred onto the shell, for
 * the same reason the crease field is: decimation destroys the UVs and the
 * fine material boundaries before it destroys anything else.
 *
 * Two layouts come out of this, and which one an asset gets is measured, not
 * chosen:
 *
 *   family-split  the asset has genuinely different materials. Palette index
 *                 becomes (family<<2)|shade, so the two shade bits sit in
 *                 tile bitplanes 0-1 and the two family bits in 2-3. The
 *                 compositor has to emit a family plane, which is new
 *                 information and therefore has a real cost.
 *
 *   mono-ramp     the asset has one hue. Splitting the palette into families
 *                 would spend two bitplanes encoding a distinction that does
 *                 not exist. Instead the whole budget becomes one ramp in that
 *                 hue, mapped straight onto the shade codes the compositor
 *                 ALREADY emits -- so nothing about the tile vocabulary
 *                 changes and the colour costs 32 bytes of palette RAM.
 */
let visualHue=null, paletteReport=null;
if(hueFamilies>0){
  const srcAlbedo=await collectWorldAlbedo(source);
  const srcAlbedoPos=normalizedPositions(
    {xyz:srcAlbedo.xyz,vertexCount:srcAlbedo.vertexCount},up,norm);
  const shellRgb=transferVec3ToShell(srcAlbedoPos,srcAlbedo.rgb,
    srcAlbedo.vertexCount,visualPos,visualGeom.vertexCount,recessRadius);
  const shellLab=new Float64Array(visualGeom.vertexCount*3);
  for(let i=0;i<visualGeom.vertexCount;++i){
    const lab=linearToOklab(shellRgb[i*3],shellRgb[i*3+1],shellRgb[i*3+2]);
    shellLab[i*3]=lab[0];shellLab[i*3+1]=lab[1];shellLab[i*3+2]=lab[2];
  }
  const areas=vertexAreas(visualGeom,visualQ8);
  const km=kmeansOklab(shellLab,areas,hueFamilies);

  /* Order families by the surface area they cover, descending. Family 0 is
   * therefore the dominant material -- which matters because family 0 is the
   * one that shares palette index 0 with the transparent/void stop, so it is
   * the family whose darkest shade is cheapest to encode. */
  const covered=km.centers.map(()=>0);
  for(let i=0;i<visualGeom.vertexCount;++i)covered[km.assign[i]]+=areas[i];
  const rank=km.centers.map((_,c)=>c).sort((a,b)=>covered[b]-covered[a]);
  const remap=new Int32Array(km.centers.length);
  rank.forEach((oldIdx,newIdx)=>{remap[oldIdx]=newIdx;});
  let centers=rank.map(c=>km.centers[c]);
  let areaShare=rank.map(c=>covered[c]);
  const totalArea=areaShare.reduce((a,b)=>a+b,0)||1;

  /* Do the families differ in MATERIAL, or only in how much light the source
   * texture already had baked onto them? See materialResidual(): chroma that
   * tracks lightness is shading, and only what is left over is a second
   * material. The threshold is one 4-bit chroma grid step, i.e. "a difference
   * the hardware could actually draw". */
  const gridStep=chromaGridStep();
  const fit=materialResidual(centers,areaShare);
  const polychrome=fit.residual>=gridStep;
  const layout=polychrome?'family-split':'mono-ramp';
  const shades=polychrome?hueShades:MONO_RAMP_STOPS;

  let residual=0;
  if(polychrome){
    visualHue=new Uint8Array(visualGeom.vertexCount);
    for(let i=0;i<visualGeom.vertexCount;++i)visualHue[i]=remap[km.assign[i]];
    for(let i=0;i<visualGeom.vertexCount;++i)
      residual+=oklabDistance([shellLab[i*3],shellLab[i*3+1],shellLab[i*3+2]],
                              centers[visualHue[i]])*areas[i];
    residual/=totalArea;
  }else{
    /* One family: the area-weighted mean of the whole surface, which is the
     * hue the ramp is painted in. The lightness variation the clustering found
     * is not discarded -- it is exactly what the shade ramp is for. */
    const mean=[0,0,0];
    for(let i=0;i<visualGeom.vertexCount;++i)
      for(let k=0;k<3;++k)mean[k]+=shellLab[i*3+k]*areas[i];
    for(let k=0;k<3;++k)mean[k]/=totalArea;
    centers=[mean]; areaShare=[totalArea];
    for(let i=0;i<visualGeom.vertexCount;++i){
      const da=shellLab[i*3+1]-mean[1], db=shellLab[i*3+2]-mean[2];
      residual+=Math.hypot(da,db)*areas[i];
    }
    residual/=totalArea;
  }

  const meanL=centers.reduce((a,c,i)=>a+c[0]*areaShare[i],0)/totalArea;
  const families=centers.map((c,f)=>{
    const ramp=synthesizeRamp(c,shades,polychrome?meanL:null);
    const solved=solveRamp(ramp);
    /* Where the ramp stop lives in the 16-entry sprite palette.
     *
     * family-split: (family<<2)|shade, with shade 0 collapsing to the shared
     *   transparent/void entry 0 in every family.
     * mono-ramp: the compositor's existing brightness ramp, in BRIGHTNESS
     *   order -- SEM_FAR, SEM_FAR_MID, SEM_MID, SEM_MID_NEAR, SEM_NEAR are
     *   enum values 3,6,4,7,5, appended to the enum out of order. Writing the
     *   ramp into those five entries recolours the hero without changing a
     *   single pixel code, which is the whole point of this branch.
     */
    const indices=polychrome
      ? solved.map((_,s)=>s===0?0:(f<<2)|s)
      : MONO_RAMP_PALETTE_INDICES.slice(0,solved.length);
    return {
      family:f, areaShare:areaShare[f]/totalArea, centerOklab:c,
      centerSrgb8:solved[Math.min(2,solved.length-1)].srgb8,
      indices, stops:solved
    };
  });

  const flat=families.flatMap(f=>f.stops);
  const meanQuant=flat.reduce((a,s)=>a+s.nearestErr,0)/flat.length;
  const meanStatic=flat.reduce((a,s)=>a+s.staticErr,0)/flat.length;
  const meanInter=flat.reduce((a,s)=>a+s.interleave.err,0)/flat.length;
  paletteReport={
    layout, requestedFamilies:hueFamilies, families:centers.length, shades,
    source:srcAlbedo.meta,
    materialResidualOklab:fit.residual, chromaGridStepOklab:gridStep,
    chromaPerLightness:[fit.ka,fit.kb],
    polychrome,
    familyResidualOklab:residual,
    meanStopQuantErrOklab:meanQuant,
    meanStopStaticErrOklab:meanStatic,
    meanStopInterleavedErrOklab:meanInter,
    interleaveGainPct:meanQuant>0?100*(1-meanInter/meanQuant):0,
    unsafeInterleaveStops:flat.filter(s=>!s.interleave.safe).length,
    inertia:km.inertia,
    entries:families
  };
}

const visualRecess=new Uint8Array(visualGeom.vertexCount);
for(let i=0;i<visualGeom.vertexCount;++i){
  const g=geomField[i],m=visualMaterial[i];
  let r=g;
  if(shadingSource==='hybrid'){
    /* Union-like multiplicative occlusion. It preserves geometric curvature
     * while letting texture-derived folds increase attenuation without the
     * hard clipping of a simple additive sum. */
    r=1-(1-g)*(1-materialStrength*m);
  }else if(shadingSource==='material') r=m;
  visualRecess[i]=Math.max(0,Math.min(255,Math.round(clamp01(r)*255)));
}
const recessStats=(()=>{
  const s=Array.from(visualConc).sort((a,b)=>a-b);
  const q=p=>s[Math.min(s.length-1,Math.max(0,Math.round(p*(s.length-1))))];
  return {radius:recessRadius,max:concMax,
          p10:q(0.1),p50:q(0.5),p75:q(0.75),p90:q(0.9),p97:q(0.97),p100:q(1)};
})();
const materialStats=sourceMaterial ? {
  ...sourceMaterial.meta,
  blurSigma:materialBlur,
  hybridStrength:materialStrength,
  shellP50:percentile(visualMaterial,0.50),
  shellP90:percentile(visualMaterial,0.90),
  shellP97:percentile(visualMaterial,0.97),
  shellMax:visualMaterial.reduce((m,v)=>v>m?v:m,0)
} : null;

const header=`/* Generated by tools/glb_rmb/convert.mjs. DO NOT HAND EDIT.
 * source: ${path.basename(input)}
 * shading source: ${shadingSource}
 * source vertices/triangles: ${sourceGeom.vertexCount}/${sourceGeom.triangleCount}
 * source mapped bounds: [${b.min.map(v=>v.toFixed(6)).join(', ')}] .. [${b.max.map(v=>v.toFixed(6)).join(', ')}]
 * normalization: up=${up}, target_height=${height}, scale=${norm.scale}
 * visual vertices/triangles: ${visualGeom.vertexCount}/${visualGeom.triangleCount}
 * lighting vertices/triangles: ${lightingGeom.vertexCount}/${lightingGeom.triangleCount}
 * shadow vertices/triangles: ${shadowGeom.vertexCount}/${shadowGeom.triangleCount}
 * crease source: ${srcWelded.vertexCount} welded source vertices,
 *   transfer radius ${recessRadius}, positive-concavity max ${concMax.toFixed(5)}
 * material form: ${materialStats ? `base=${materialStats.baseColor} normal=${materialStats.normal} ao=${materialStats.occlusion}` : 'disabled'}${paletteReport ? `
 * palette: ${paletteReport.layout}, ${paletteReport.families} family/families x ${paletteReport.shades} shades, chroma residual ${paletteReport.familyResidualOklab.toFixed(4)} Oklab` : ''}
 */
`;
const body=header+
  emitMesh(name,'visual',visualGeom,visualQ8)+
  emitRecess(name,'visual',visualRecess)+
  (visualHue?emitHue(name,'visual',visualHue):'')+'\n'+
  emitMesh(name,'lighting',lightingGeom,lightingQ8)+'\n'+
  emitMesh(name,'shadow',shadowGeom,shadowQ8);
await fs.mkdir(path.dirname(output),{recursive:true});
await fs.writeFile(output,body,'utf8');
if(paletteReport){
  const palettePath=output.replace(/\.inc$/,'')+'_palette.json';
  await fs.writeFile(palettePath,JSON.stringify(paletteReport,null,2),'utf8');
  paletteReport.path=path.resolve(palettePath);
}

const stats={
  input:path.resolve(input), output:path.resolve(output), name, up, targetHeight:height,
  shadingSource, materialStrength, materialBlur,
  source:{vertices:sourceGeom.vertexCount,triangles:sourceGeom.triangleCount,bounds:b},
  visual:{vertices:visualGeom.vertexCount,triangles:visualGeom.triangleCount,target:visualTarget},
  lighting:{vertices:lightingGeom.vertexCount,triangles:lightingGeom.triangleCount,target:lightingTarget},
  shadow:{vertices:shadowGeom.vertexCount,triangles:shadowGeom.triangleCount,target:shadowTarget},
  q8:{scale:norm.scale,centerXY:[norm.cx,norm.cy],baseZ:norm.z0},
  recess:recessStats,
  materialForm:materialStats,
  palette:paletteReport
};
console.log(JSON.stringify(stats,null,2));
