/*
 * Source-mesh recess extraction.
 *
 * The visual shell is decimated from ~500k triangles to a few thousand, and
 * decimation removes high-curvature detail first -- which is precisely the
 * detail that describes folds. Measuring curvature on the shell therefore
 * measures a surface whose creases have already been smoothed away: on this
 * asset the shell's concavity field came out negative (convex) for 89% of the
 * surface with a maximum of 0.34, and the crease pass it drove touched 482
 * pixels across 2,500 frames.
 *
 * So curvature is measured on the FULL-RESOLUTION source and transferred onto
 * the shell, using the source as a stencil. The transfer radius is deliberately
 * a WORLD-SPACE distance rather than a topological neighbourhood: the shell can
 * only draw a crease about one shell-triangle wide, so the field is filtered at
 * the scale the shell can actually represent instead of at source-vertex scale.
 *
 * Convention matches tools/glb_rmb/analyze_seams.mjs:
 *   delta     = centroid(one-ring) - vertex
 *   concavity = dot(delta, outward normal) / mean ring edge length
 *   positive  = concave crevice/seam, negative = convex ridge.
 */
const EPS=1e-12;

export function computeSourceConcavity(xyz, indices, vertexCount, triCount) {
  const nx=new Float64Array(vertexCount), ny=new Float64Array(vertexCount), nz=new Float64Array(vertexCount);
  const cx=new Float64Array(vertexCount), cy=new Float64Array(vertexCount), cz=new Float64Array(vertexCount);
  const scale=new Float64Array(vertexCount), w=new Float64Array(vertexCount);

  for(let t=0;t<triCount;++t){
    const a=indices[t*3], b=indices[t*3+1], c=indices[t*3+2];
    const ax=xyz[a*3],ay=xyz[a*3+1],az=xyz[a*3+2];
    const bx=xyz[b*3],by=xyz[b*3+1],bz=xyz[b*3+2];
    const ccx=xyz[c*3],ccy=xyz[c*3+1],ccz=xyz[c*3+2];
    const e1x=bx-ax,e1y=by-ay,e1z=bz-az;
    const e2x=ccx-ax,e2y=ccy-ay,e2z=ccz-az;
    // Unnormalized cross product is already area weighted.
    const fx=e1y*e2z-e1z*e2y, fy=e1z*e2x-e1x*e2z, fz=e1x*e2y-e1y*e2x;
    const tri=[a,b,c];
    for(let k=0;k<3;++k){
      const v=tri[k];
      nx[v]+=fx; ny[v]+=fy; nz[v]+=fz;
      for(let m=1;m<3;++m){
        const u=tri[(k+m)%3];
        const dx=xyz[u*3]-xyz[v*3], dy=xyz[u*3+1]-xyz[v*3+1], dz=xyz[u*3+2]-xyz[v*3+2];
        cx[v]+=xyz[u*3]; cy[v]+=xyz[u*3+1]; cz[v]+=xyz[u*3+2];
        scale[v]+=Math.hypot(dx,dy,dz);
        w[v]+=1;
      }
    }
  }

  const conc=new Float32Array(vertexCount);
  for(let v=0;v<vertexCount;++v){
    if(w[v]<1) continue;
    const nl=Math.hypot(nx[v],ny[v],nz[v]);
    if(nl<EPS) continue;
    const ux=nx[v]/nl, uy=ny[v]/nl, uz=nz[v]/nl;
    const dx=cx[v]/w[v]-xyz[v*3], dy=cy[v]/w[v]-xyz[v*3+1], dz=cz[v]/w[v]-xyz[v*3+2];
    const sc=scale[v]/w[v];
    conc[v]= sc>EPS ? (dx*ux+dy*uy+dz*uz)/sc : 0;
  }
  return conc;
}

/*
 * Transfer the source field onto shell vertices by averaging every source
 * vertex within `radius`. A uniform grid keeps this linear; at 1.5M source
 * vertices a naive scan would be quadratic.
 */
export function transferToShell(srcXyz, srcConc, srcCount, dstXyz, dstCount, radius) {
  let mnx=Infinity,mny=Infinity,mnz=Infinity,mxx=-Infinity,mxy=-Infinity,mxz=-Infinity;
  for(let i=0;i<srcCount;++i){
    const x=srcXyz[i*3],y=srcXyz[i*3+1],z=srcXyz[i*3+2];
    if(x<mnx)mnx=x; if(y<mny)mny=y; if(z<mnz)mnz=z;
    if(x>mxx)mxx=x; if(y>mxy)mxy=y; if(z>mxz)mxz=z;
  }
  const cell=Math.max(radius,1e-6);
  const gx=Math.max(1,Math.ceil((mxx-mnx)/cell)+1);
  const gy=Math.max(1,Math.ceil((mxy-mny)/cell)+1);
  const gz=Math.max(1,Math.ceil((mxz-mnz)/cell)+1);
  const nCells=gx*gy*gz;
  const counts=new Int32Array(nCells+1);
  const key=new Int32Array(srcCount);
  const ci=(x,y,z)=>{
    let i=Math.floor((x-mnx)/cell), j=Math.floor((y-mny)/cell), k=Math.floor((z-mnz)/cell);
    if(i<0)i=0; if(j<0)j=0; if(k<0)k=0;
    if(i>=gx)i=gx-1; if(j>=gy)j=gy-1; if(k>=gz)k=gz-1;
    return (k*gy+j)*gx+i;
  };
  for(let i=0;i<srcCount;++i){ const c=ci(srcXyz[i*3],srcXyz[i*3+1],srcXyz[i*3+2]); key[i]=c; counts[c+1]++; }
  for(let c=0;c<nCells;++c) counts[c+1]+=counts[c];
  const order=new Int32Array(srcCount);
  const fill=counts.slice(0,nCells);
  for(let i=0;i<srcCount;++i) order[fill[key[i]]++]=i;

  const out=new Float32Array(dstCount);
  const r2=radius*radius;
  for(let d=0;d<dstCount;++d){
    const px=dstXyz[d*3],py=dstXyz[d*3+1],pz=dstXyz[d*3+2];
    let i0=Math.floor((px-mnx)/cell), j0=Math.floor((py-mny)/cell), k0=Math.floor((pz-mnz)/cell);
    let sum=0,n=0,best=0,bestD=Infinity;
    for(let k=k0-1;k<=k0+1;++k){ if(k<0||k>=gz)continue;
    for(let j=j0-1;j<=j0+1;++j){ if(j<0||j>=gy)continue;
    for(let i=i0-1;i<=i0+1;++i){ if(i<0||i>=gx)continue;
      const c=(k*gy+j)*gx+i;
      for(let s=counts[c];s<counts[c+1];++s){
        const v=order[s];
        const dx=srcXyz[v*3]-px, dy=srcXyz[v*3+1]-py, dz=srcXyz[v*3+2]-pz;
        const dd=dx*dx+dy*dy+dz*dz;
        if(dd<bestD){ bestD=dd; best=srcConc[v]; }
        if(dd<=r2){ sum+=srcConc[v]; ++n; }
      }
    }}}
    /* Fall back to the nearest source vertex when the radius caught nothing,
     * so a shell vertex sitting slightly outside the source hull still gets a
     * defined value instead of a hole in the field. */
    out[d]= n>0 ? sum/n : best;
  }
  return out;
}
