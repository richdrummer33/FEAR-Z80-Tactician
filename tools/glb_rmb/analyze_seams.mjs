#!/usr/bin/env node
/*
 * Source-mesh cavity/seam extractor for GG hero baking.
 *
 * Convention (pinned deliberately):
 *   delta = centroid(ring) - vertex
 *   concavity = dot(delta, outward_normal) / local_scale
 *   positive = concave crevice/seam, negative = convex ridge.
 *
 * Detection is performed on the welded SOURCE mesh, not the decimated visual
 * shell. The surviving scalar field is transferred to the visual shell by an
 * approximate nearest-surface-point query, then emitted as sparse clipped
 * overlay geometry for 25/50/75% ordered-dither darkness.
 */
import fs from 'node:fs/promises';
import path from 'node:path';
import { NodeIO } from '@gltf-transform/core';
import { ALL_EXTENSIONS } from '@gltf-transform/extensions';
import { prune, simplify, weld } from '@gltf-transform/functions';
import draco3d from 'draco3dgltf';
import { MeshoptDecoder, MeshoptSimplifier } from 'meshoptimizer';

const EPS=1e-12;
function fail(s){console.error('fatal:',s);process.exit(2);}
function av(a,n,f){const i=a.indexOf(n);return i>=0&&i+1<a.length?a[i+1]:f;}
function vec3(s){const a=String(s).split(',').map(Number);if(a.length!==3||a.some(x=>!Number.isFinite(x)))fail('expected x,y,z');return a;}
const add=(a,b)=>[a[0]+b[0],a[1]+b[1],a[2]+b[2]];
const sub=(a,b)=>[a[0]-b[0],a[1]-b[1],a[2]-b[2]];
const mul=(a,s)=>[a[0]*s,a[1]*s,a[2]*s];
const dot=(a,b)=>a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const len=a=>Math.hypot(a[0],a[1],a[2]);
const norm=a=>{const l=len(a);return l<EPS?[0,0,0]:mul(a,1/l);};
const dist=(a,b)=>len(sub(a,b));
function transformPoint(m,p){return[
 m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12],
 m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13],
 m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]
];}
function mapAxes(p,up){
 if(up==='z')return p;
 if(up==='y')return[p[0],-p[2],p[1]];
 if(up==='x')return[p[1],p[2],p[0]];
 fail('--up must be x/y/z');
}
async function makeIO(){
 await MeshoptDecoder.ready;
 const decoder=await draco3d.createDecoderModule();
 return new NodeIO().registerExtensions(ALL_EXTENSIONS)
   .registerDependencies({'meshopt.decoder':MeshoptDecoder,'draco3d.decoder':decoder});
}
function dropAttrs(doc){
 for(const mesh of doc.getRoot().listMeshes())for(const prim of mesh.listPrimitives()){
   for(const sem of prim.listSemantics())if(sem!=='POSITION')prim.getAttribute(sem)?.dispose();
   for(const t of prim.listTargets())t.dispose();
 }
}
function collect(doc,up='z'){
 const xyz=[],idx=[];let base=0;
 const nodes=doc.getRoot().listNodes().filter(n=>n.getMesh());
 for(const node of nodes){
   const m=node.getWorldMatrix();
   for(const prim of node.getMesh().listPrimitives()){
     if(prim.getMode()!==4)fail('only TRIANGLES supported');
     const pa=prim.getAttribute('POSITION');if(!pa)continue;
     const a=pa.getArray();
     for(let i=0;i<pa.getCount();++i){
       const p=mapAxes(transformPoint(m,[a[i*3],a[i*3+1],a[i*3+2]]),up);
       xyz.push(p);
     }
     const ia=prim.getIndices();
     if(ia){const q=ia.getArray();for(let i=0;i<ia.getCount();++i)idx.push(base+Number(q[i]));}
     else for(let i=0;i<pa.getCount();++i)idx.push(base+i);
     base+=pa.getCount();
   }
 }
 return{xyz,idx,triCount:idx.length/3};
}
async function sourceGeom(io,input,up){
 const doc=await io.read(input);dropAttrs(doc);await doc.transform(weld(),prune());
 return collect(doc,up);
}
async function simplifiedGeom(io,input,up,target){
 const doc=await io.read(input);dropAttrs(doc);await doc.transform(weld());
 let tris=0;for(const m of doc.getRoot().listMeshes())for(const p of m.listPrimitives()){
   const a=p.getIndices(),v=p.getAttribute('POSITION');tris+=Math.floor((a?a.getCount():v.getCount())/3);
 }
 if(tris>target){
   await MeshoptSimplifier.ready;
   await doc.transform(simplify({simplifier:MeshoptSimplifier,ratio:Math.max(1e-7,target/tris),error:1,lockBorder:false}),prune());
 }else await doc.transform(prune());
 return collect(doc,up);
}
function bounds(g){
 const mn=[Infinity,Infinity,Infinity],mx=[-Infinity,-Infinity,-Infinity];
 for(const p of g.xyz)for(let k=0;k<3;k++){mn[k]=Math.min(mn[k],p[k]);mx[k]=Math.max(mx[k],p[k]);}
 return{mn,mx,diag:dist(mn,mx),ctr:mul(add(mn,mx),.5)};
}
function normalizeGeom(g,b,height){
 const scale=height/(b.mx[2]-b.mn[2]),cx=(b.mn[0]+b.mx[0])*.5,cy=(b.mn[1]+b.mx[1])*.5,z0=b.mn[2];
 return{xyz:g.xyz.map(p=>[(p[0]-cx)*scale,(p[1]-cy)*scale,(p[2]-z0)*scale]),idx:g.idx,triCount:g.triCount,scale};
}
function buildGraph(g){
 const n=g.xyz.length,adj=Array.from({length:n},()=>[]);
 const va=Array.from({length:n},()=>[0,0,0]),vw=new Float64Array(n),meanEdge=new Float64Array(n);
 for(let t=0;t<g.idx.length;t+=3){
   const ids=[g.idx[t],g.idx[t+1],g.idx[t+2]],p=ids.map(i=>g.xyz[i]);
   const cr=cross(sub(p[1],p[0]),sub(p[2],p[0])),a=.5*len(cr);
   for(let j=0;j<3;j++){va[ids[j]]=add(va[ids[j]],cr);vw[ids[j]]+=a/3;}
   for(const [u,v] of [[ids[0],ids[1]],[ids[1],ids[2]],[ids[2],ids[0]]]){adj[u].push(v);adj[v].push(u);}
 }
 for(let i=0;i<n;i++){
   adj[i].sort((a,b)=>a-b);let w=0,last=-1;
   for(const v of adj[i])if(v!==last){adj[i][w++]=v;last=v;}adj[i].length=w;
   let s=0;for(const v of adj[i])s+=dist(g.xyz[i],g.xyz[v]);meanEdge[i]=w?s/w:0;
   va[i]=norm(va[i]);
 }
 const ctr=bounds(g).ctr;
 let orient=0,weight=0;for(let i=0;i<n;i++){orient+=dot(va[i],sub(g.xyz[i],ctr))*vw[i];weight+=vw[i];}
 if(orient<0)for(let i=0;i<n;i++)va[i]=mul(va[i],-1);
 return{adj,normals:va,areaWeight:vw,meanEdge};
}
function ringFrontier(adj,start,hops){
 let cur=[start],seen=new Set([start]);
 for(let h=0;h<hops;h++){const next=[];for(const u of cur)for(const v of adj[u])if(!seen.has(v)){seen.add(v);next.push(v);}cur=next;if(!cur.length)break;}
 return cur;
}
function ringMetrics(g,graph,i,hops){
 const ring=ringFrontier(graph.adj,i,hops);if(ring.length<3)return{conc:0,aniso:0,tangent:[0,0,0]};
 let ctr=[0,0,0];for(const j of ring)ctr=add(ctr,g.xyz[j]);ctr=mul(ctr,1/ring.length);
 const scale=Math.max(EPS,graph.meanEdge[i]*hops);
 const delta=sub(ctr,g.xyz[i]);
 const conc=dot(delta,graph.normals[i])/scale;
 const n=graph.normals[i],helper=Math.abs(n[2])<.85?[0,0,1]:[1,0,0],u=norm(cross(n,helper)),v=cross(n,u);
 let xx=0,xy=0,yy=0;
 for(const j of ring){const d=sub(g.xyz[j],g.xyz[i]),x=dot(d,u),y=dot(d,v);xx+=x*x;xy+=x*y;yy+=y*y;}
 xx/=ring.length;xy/=ring.length;yy/=ring.length;
 const tr=xx+yy,disc=Math.sqrt(Math.max(0,(xx-yy)*(xx-yy)+4*xy*xy)),lmax=(tr+disc)*.5,lmin=(tr-disc)*.5;
 const aniso=tr>EPS?(lmax-lmin)/tr:0;
 let ev2;
 if(Math.abs(xy)>EPS)ev2=norm([xy,lmin-xx,0]);
 else ev2=xx<yy?[1,0,0]:[0,1,0];
 const tangent=norm(add(mul(u,ev2[0]),mul(v,ev2[1])));
 return{conc,aniso,tangent};
}
function weightedPercentile(vals,weights,p){
 const a=[];let total=0;for(let i=0;i<vals.length;i++)if(Number.isFinite(vals[i])&&vals[i]>0&&weights[i]>0){a.push([vals[i],weights[i]]);total+=weights[i];}
 if(!a.length)return 0;a.sort((x,y)=>x[0]-y[0]);const goal=total*p;let s=0;
 for(const q of a){s+=q[1];if(s>=goal)return q[0];}return a[a.length-1][0];
}
function percentile(a,p){const b=a.filter(Number.isFinite).sort((x,y)=>x-y);return b.length?b[Math.min(b.length-1,Math.floor((b.length-1)*p))]:0;}
function componentPCA(g,verts){
 let ctr=[0,0,0];for(const i of verts)ctr=add(ctr,g.xyz[i]);ctr=mul(ctr,1/verts.length);
 let xx=0,xy=0,xz=0,yy=0,yz=0,zz=0;
 for(const i of verts){const d=sub(g.xyz[i],ctr);xx+=d[0]*d[0];xy+=d[0]*d[1];xz+=d[0]*d[2];yy+=d[1]*d[1];yz+=d[1]*d[2];zz+=d[2]*d[2];}
 // power iteration major axis; width from RMS residual.
 let a=norm([1,.7,.3]);for(let k=0;k<12;k++)a=norm([xx*a[0]+xy*a[1]+xz*a[2],xy*a[0]+yy*a[1]+yz*a[2],xz*a[0]+yz*a[1]+zz*a[2]]);
 let min=Infinity,max=-Infinity,res=0;for(const i of verts){const d=sub(g.xyz[i],ctr),q=dot(d,a);min=Math.min(min,q);max=Math.max(max,q);res+=Math.max(0,dot(d,d)-q*q);}
 return{ctr,axis:a,length:max-min,width:Math.sqrt(res/Math.max(1,verts.length))};
}
function rayTri(o,d,a,b,c){
 const e1=sub(b,a),e2=sub(c,a),p=cross(d,e2),det=dot(e1,p);if(Math.abs(det)<EPS)return false;
 const inv=1/det,tv=sub(o,a),u=dot(tv,p)*inv;if(u<0||u>1)return false;
 const q=cross(tv,e1),v=dot(d,q)*inv;if(v<0||u+v>1)return false;
 const t=dot(e2,q)*inv;return t>1e-5&&t<1-1e-5;
}
function visibleToLight(p,light,shadow){
 const d=sub(p,light);for(let t=0;t<shadow.idx.length;t+=3){
   if(rayTri(light,d,shadow.xyz[shadow.idx[t]],shadow.xyz[shadow.idx[t+1]],shadow.xyz[shadow.idx[t+2]]))return false;
 }return true;
}
function yawVec(dx,dy){const l=Math.hypot(dx,dy)||1;return[dx/l,dy/l];}
function projectedImportance(comp,g,scale,offset){
 let sum=0;
 for(let ci=0;ci<12;ci++){
   const a=(110-220*(ci/11))*Math.PI/180,cam=[78+22*Math.cos(a),24+27*Math.sin(a),15],target=[78,24,15];
   const f=yawVec(target[0]-cam[0],target[1]-cam[1]),r=[-f[1],f[0]];
   let mnx=Infinity,mxx=-Infinity,mny=Infinity,mxy=-Infinity,seen=0;
   for(const vi of comp.verts){
     const lp=g.xyz[vi],p=[offset[0]+lp[0]*scale,offset[1]+lp[1]*scale,offset[2]+lp[2]*scale];
     const dx=p[0]-cam[0],dy=p[1]-cam[1],dep=dx*f[0]+dy*f[1];if(dep<=.5)continue;
     const lat=dx*r[0]+dy*r[1],sx=80+lat*80/dep,sy=72-(p[2]-cam[2])*80/dep;
     mnx=Math.min(mnx,sx);mxx=Math.max(mxx,sx);mny=Math.min(mny,sy);mxy=Math.max(mxy,sy);seen++;
   }
   if(seen)sum+=Math.max(0,Math.min(160,mxx)-Math.max(0,mnx))*Math.max(0,Math.min(144,mxy)-Math.max(0,mny));
 }
 return sum/12;
}
class Heap{constructor(){this.a=[];}push(d,v){let i=this.a.length;this.a.push([d,v]);while(i){let p=(i-1)>>1;if(this.a[p][0]<=d)break;this.a[i]=this.a[p];i=p;}this.a[i]=[d,v];}pop(){if(!this.a.length)return null;const r=this.a[0],x=this.a.pop();if(this.a.length){let i=0;while(true){let l=i*2+1;if(l>=this.a.length)break;let rr=l+1,j=rr<this.a.length&&this.a[rr][0]<this.a[l][0]?rr:l;if(this.a[j][0]>=x[0])break;this.a[i]=this.a[j];i=j;}this.a[i]=x;}return r;}}
function geodesicField(g,graph,seeds,cutoff){
 const n=g.xyz.length,D=new Float64Array(n);D.fill(Infinity);const h=new Heap();
 for(const s of seeds){D[s]=0;h.push(0,s);}
 while(h.a.length){const q=h.pop(),du=q[0],u=q[1];if(du!==D[u]||du>cutoff)continue;
   for(const v of graph.adj[u]){const nd=du+dist(g.xyz[u],g.xyz[v]);if(nd<D[v]&&nd<=cutoff){D[v]=nd;h.push(nd,v);}}
 }return D;
}
function closestPointTri(p,a,b,c){
 const ab=sub(b,a),ac=sub(c,a),ap=sub(p,a),d1=dot(ab,ap),d2=dot(ac,ap);
 if(d1<=0&&d2<=0)return{p:a,b:[1,0,0]};
 const bp=sub(p,b),d3=dot(ab,bp),d4=dot(ac,bp);if(d3>=0&&d4<=d3)return{p:b,b:[0,1,0]};
 const vc=d1*d4-d3*d2;if(vc<=0&&d1>=0&&d3<=0){const v=d1/(d1-d3);return{p:add(a,mul(ab,v)),b:[1-v,v,0]};}
 const cp=sub(p,c),d5=dot(ab,cp),d6=dot(ac,cp);if(d6>=0&&d5<=d6)return{p:c,b:[0,0,1]};
 const vb=d5*d2-d1*d6;if(vb<=0&&d2>=0&&d6<=0){const w=d2/(d2-d6);return{p:add(a,mul(ac,w)),b:[1-w,0,w]};}
 const va=d3*d6-d5*d4;if(va<=0&&(d4-d3)>=0&&(d5-d6)>=0){const w=(d4-d3)/((d4-d3)+(d5-d6));return{p:add(b,mul(sub(c,b),w)),b:[0,1-w,w]};}
 const den=1/(va+vb+vc),v=vb*den,w=vc*den;return{p:add(a,add(mul(ab,v),mul(ac,w))),b:[1-v-w,v,w]};
}
function surfaceMap(src,vis){
 const b=bounds(src),cell=Math.max(EPS,b.diag/48),map=new Map(),key=(x,y,z)=>x+','+y+','+z;
 for(let t=0;t<src.idx.length;t+=3){
   const a=src.xyz[src.idx[t]],bb=src.xyz[src.idx[t+1]],c=src.xyz[src.idx[t+2]],ctr=mul(add(add(a,bb),c),1/3);
   const k=key(Math.floor((ctr[0]-b.mn[0])/cell),Math.floor((ctr[1]-b.mn[1])/cell),Math.floor((ctr[2]-b.mn[2])/cell));
   if(!map.has(k))map.set(k,[]);map.get(k).push(t/3);
 }
 const out=[];
 for(const p of vis.xyz){let best=null,bd=Infinity;
   const ix=Math.floor((p[0]-b.mn[0])/cell),iy=Math.floor((p[1]-b.mn[1])/cell),iz=Math.floor((p[2]-b.mn[2])/cell);
   for(let rad=0;rad<=4&&best===null;rad++)for(let dx=-rad;dx<=rad;dx++)for(let dy=-rad;dy<=rad;dy++)for(let dz=-rad;dz<=rad;dz++){
     if(rad&&Math.max(Math.abs(dx),Math.abs(dy),Math.abs(dz))!==rad)continue;
     const list=map.get(key(ix+dx,iy+dy,iz+dz));if(!list)continue;
     for(const ti of list){const o=ti*3,ids=[src.idx[o],src.idx[o+1],src.idx[o+2]],q=closestPointTri(p,src.xyz[ids[0]],src.xyz[ids[1]],src.xyz[ids[2]]),dd=dist(p,q.p);if(dd<bd){bd=dd;best={ids,bary:q.b};}}
   }
   if(!best){ // rare fallback: nearest source vertex
     let bi=0;bd=Infinity;for(let i=0;i<src.xyz.length;i++){const d=dist(p,src.xyz[i]);if(d<bd){bd=d;bi=i;}}best={ids:[bi,bi,bi],bary:[1,0,0]};
   }
   out.push(best);
 }
 return out;
}
function q8(p){return p.map(x=>{const v=Math.round(x*256);if(v<-32768||v>32767)fail('Q8 overflow');return v;});}
function compactLayer(vis,triIds){
 const remap=new Map(),xyz=[],idx=[];
 for(const ti of triIds)for(let k=0;k<3;k++){const old=vis.idx[ti*3+k];if(!remap.has(old)){remap.set(old,remap.size);xyz.push(...q8(vis.xyz[old]));}idx.push(remap.get(old));}
 return{xyz,idx,verts:remap.size,tris:triIds.length};
}
function carr(type,name,a,per=12){let s=`static const ${type} ${name}[] = {\n`;for(let i=0;i<a.length;i+=per)s+='  '+a.slice(i,i+per).join(', ')+(i+per<a.length?',':'')+'\n';return s+'};\n';}

const args=process.argv.slice(2);if(args.length<2)fail('usage: analyze_seams.mjs INPUT.glb OUTPUT.inc [options]');
const input=args[0],output=args[1],up=av(args,'--up','z'),height=Number(av(args,'--height','19'));
const name=String(av(args,'--name','doomguy')).replace(/[^A-Za-z0-9_]/g,'_');
const macro=name.toUpperCase();
const visualTarget=Number(av(args,'--visual-tris','5200')),shadowTarget=Number(av(args,'--shadow-tris','200'));
const topN=Math.max(1,Number(av(args,'--max-components','12'))|0);
const modelScale=Number(av(args,'--model-scale','1.35')),offset=vec3(av(args,'--model-offset','78,24,3')),lightWorld=vec3(av(args,'--light-world','62,-96,18'));
const io=await makeIO();
const srcRaw=await sourceGeom(io,input,up),b0=bounds(srcRaw),src=normalizeGeom(srcRaw,b0,height);
const visRaw=await simplifiedGeom(io,input,up,visualTarget),vis=normalizeGeom(visRaw,b0,height);
const shRaw=await simplifiedGeom(io,input,up,shadowTarget),shadow=normalizeGeom(shRaw,b0,height);
const graph=buildGraph(src),n=src.xyz.length;
const small=new Float64Array(n),large=new Float64Array(n),aniso=new Float64Array(n),tangents=Array(n);
for(let i=0;i<n;i++){const m=ringMetrics(src,graph,i,1);small[i]=m.conc;aniso[i]=m.aniso;tangents[i]=m.tangent;}
const high=weightedPercentile(small,graph.areaWeight,.98),low=weightedPercentile(small,graph.areaWeight,.94);
const pool=[];for(let i=0;i<n;i++)if(small[i]>=low*.8)pool.push(i);
for(const i of pool){const m=ringMetrics(src,graph,i,3);large[i]=m.conc;aniso[i]=Math.max(aniso[i],m.aniso);tangents[i]=m.tangent;}
const anSeed=percentile(pool.filter(i=>small[i]>=high).map(i=>aniso[i]),.55),anLow=percentile(pool.map(i=>aniso[i]),.35);
const seed=new Uint8Array(n),keep=new Uint8Array(n);
for(const i of pool){
 const coherent=large[i]>0&&large[i]>=small[i]*.22;
 if(coherent&&small[i]>=high&&aniso[i]>=anSeed)seed[i]=1;
 if(coherent&&small[i]>=low&&aniso[i]>=anLow)keep[i]=1;
}
const seen=new Uint8Array(n),comps=[];
for(let i=0;i<n;i++)if(seed[i]&&!seen[i]){
 const q=[i],verts=[];seen[i]=1;
 for(let qi=0;qi<q.length;qi++){const u=q[qi];verts.push(u);for(const v of graph.adj[u])if(keep[v]&&!seen[v]){seen[v]=1;q.push(v);}}
 if(verts.length<4)continue;
 const pca=componentPCA(src,verts),meanE=verts.reduce((s,v)=>s+graph.meanEdge[v],0)/verts.length;
 const minLen=Math.max(meanE*3,bounds(src).diag*.01),ratio=pca.width>EPS?pca.length/pca.width:99;
 if(pca.length<minLen||ratio<1.8)continue;
 let depth=0,nrm=[0,0,0];for(const v of verts){depth+=Math.max(0,(small[v]+large[v])*.5);nrm=add(nrm,mul(graph.normals[v],graph.areaWeight[v]||1));}
 depth/=verts.length;nrm=norm(nrm);
 const worldCtr=add(offset,mul(pca.ctr,modelScale)),ld=norm(sub(lightWorld,worldCtr)),nd=Math.max(0,dot(nrm,ld));
 const exposed=nd>0&&visibleToLight(worldCtr,lightWorld,{xyz:shadow.xyz.map(p=>add(offset,mul(p,modelScale))),idx:shadow.idx});
 if(!exposed)continue;
 const screen=projectedImportance({verts},src,modelScale,offset),score=pca.length*depth*screen*nd;
 comps.push({verts,pca,depth,normal:nrm,exposure:nd,screen,score,meanE});
}
comps.sort((a,b)=>b.score-a.score);comps.length=Math.min(topN,comps.length);
const smap=surfaceMap(src,vis),layers=[];
for(let rank=0;rank<comps.length;rank++){
 const c=comps[rank],cutoff=c.meanE*5.0,D=geodesicField(src,graph,c.verts,cutoff);
 const vd=new Float64Array(vis.xyz.length);
 for(let i=0;i<vis.xyz.length;i++){const m=smap[i];let q=0;for(let k=0;k<3;k++)q+=m.bary[k]*D[m.ids[k]];vd[i]=q;}
 const buckets=[[],[],[]];
 for(let ti=0;ti<vis.triCount;ti++){
   const a=vis.idx[ti*3],bb=vis.idx[ti*3+1],cc=vis.idx[ti*3+2],d=(vd[a]+vd[bb]+vd[cc])/3;
   if(!Number.isFinite(d)||d>cutoff)continue;
   const q=d<=c.meanE*1.5?3:(d<=c.meanE*3?2:1);
   buckets[q-1].push(ti);
 }
 for(let q=1;q<=3;q++)if(buckets[q-1].length){
   const z=compactLayer(vis,buckets[q-1]);layers.push({rank:rank+1,quarters:q,...z});
 }
}
let out=`/* Generated by analyze_seams.mjs. Positive concavity convention:
 * delta=centroid(ring)-vertex; dot(delta,outward_normal)>0 => crevice.
 * source analysis: welded source mesh, scale-normalized, 1-hop + 3-hop gated,
 * anisotropy filtered, hysteresis connected, geodesic falloff.
 */\n`;
out+=`#define ${macro}_SEAM_COMPONENT_COUNT ${comps.length}u\n`;
out+=`#ifndef RMB_GENERATED_SEAM_LAYER_DEFINED\n#define RMB_GENERATED_SEAM_LAYER_DEFINED 1\ntypedef struct RMBGeneratedSeamLayer { uint8_t rank,quarters; const int16_t *xyz; uint16_t vertex_count; const uint16_t *idx; uint16_t triangle_count; } RMBGeneratedSeamLayer;\n#endif\n`;
for(let i=0;i<layers.length;i++){const l=layers[i],nme=`${name}_seam_l${i}`;out+=carr('int16_t',nme+'_xyz_q8',l.xyz)+carr('uint16_t',nme+'_indices',l.idx);}
out+=`static const RMBGeneratedSeamLayer ${name}_seam_layers[] = {\n`;
if(layers.length){
  for(let i=0;i<layers.length;i++){const l=layers[i],nme=`${name}_seam_l${i}`;out+=`  {${l.rank}u,${l.quarters}u,${nme}_xyz_q8,${l.verts}u,${nme}_indices,${l.tris}u},\n`;}
}else{
  /* ISO C has no zero-length arrays. Emit one inert descriptor while keeping
   * the public count at zero so generic consumers compile warning-clean. */
  out+=`  {0u,4u,(const int16_t *)0,0u,(const uint16_t *)0,0u},\n`;
}
out+=`};\n#define ${macro}_SEAM_LAYER_COUNT ${layers.length}u\n`;
await fs.mkdir(path.dirname(output),{recursive:true});await fs.writeFile(output,out);
const stats={source:{vertices:n,triangles:src.triCount},visual:{vertices:vis.xyz.length,triangles:vis.triCount},shadow:{triangles:shadow.triCount},
 thresholds:{highConcavity:high,lowConcavity:low,seedAnisotropy:anSeed,lowAnisotropy:anLow},
 convention:'delta=centroid(ring)-vertex; positive dot outward normal = concave',
 components:comps.map((c,i)=>({rank:i+1,vertices:c.verts.length,pathLength:c.pca.length,width:c.pca.width,lengthWidth:c.pca.width>EPS?c.pca.length/c.pca.width:99,depth:c.depth,screenAreaRail:c.screen,directLightExposure:c.exposure,score:c.score})),
 layers:layers.map(l=>({rank:l.rank,ditherQuarters:l.quarters,vertices:l.verts,triangles:l.tris}))};
console.log(JSON.stringify(stats,null,2));
