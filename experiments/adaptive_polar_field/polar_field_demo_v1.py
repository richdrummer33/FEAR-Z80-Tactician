#!/usr/bin/env python3
import argparse, math, os, sys, time, json, gzip, subprocess, shutil, curses
from dataclasses import dataclass

W,H=160,144; HORIZON=72; FOV=math.pi/2; TAU=math.tau
GRID=16.0; ANGLE_BINS=2048
VERTS=[(16,16),(80,16),(80,36),(80,64),(80,80),(16,80),(112,36),(112,64),(112,14),(112,84),(136,6),(154,20),(176,10),(176,84)]
FULL=0; LINTEL=1; RAISED=2; RISER=3
SEGS=[(0,1,0,FULL),(1,2,0,FULL),(3,4,0,FULL),(4,5,0,FULL),(5,0,0,FULL),(2,6,-1,FULL),(7,3,-1,FULL),(8,6,0,RAISED),(7,9,0,RAISED),(8,10,1,RAISED),(10,11,0,RAISED),(11,12,1,RAISED),(12,13,0,RAISED),(13,9,0,RAISED),(2,3,1,LINTEL),(6,7,1,LINTEL),(6,7,-1,RISER)]
SOLIDS=list(range(14)); PORTALS=[14,15,16]
DEMO=[(145,0,1),(10,248,1),(10,236,1),(12,224,1),(12,212,1),(12,224,1),(10,236,1),(10,248,1),(16,0,1),(36,0,0)]
SIN=[int(round(127*math.sin(TAU*i/256))) for i in range(256)]

def wrapturn(t): return t%1.0

def ray_seg(px,py,ang,sid):
    ax,ay=VERTS[SEGS[sid][0]]; bx,by=VERTS[SEGS[sid][1]]
    dx,dy=math.cos(ang),math.sin(ang); sx,sy=bx-ax,by-ay
    den=dx*sy-dy*sx
    if abs(den)<1e-10: return None
    qx,qy=ax-px,ay-py
    t=(qx*sy-qy*sx)/den; u=(qx*dy-qy*dx)/den
    if t>1e-6 and -1e-8<=u<=1+1e-8: return t
    return None

def nearest_solid(px,py,ang):
    best=None; sid=-1
    for s in SOLIDS:
        t=ray_seg(px,py,ang,s)
        if t is not None and (best is None or t<best): best,sid=t,s
    return sid,best

def portal_hit(px,py,ang,sid,solid_t):
    t=ray_seg(px,py,ang,sid)
    return t if t is not None and (solid_t is None or t<solid_t-1e-6) else None

def line_coef(px,py,sid):
    ax,ay=VERTS[SEGS[sid][0]]; bx,by=VERTS[SEGS[sid][1]]
    nx,ny=(by-ay),-(bx-ax)
    den=nx*ax+ny*ay-(nx*px+ny*py)
    return (0.0 if abs(den)<1e-9 else 1.0/den)

def invrad_from_coef(sid,coef,ang):
    ax,ay=VERTS[SEGS[sid][0]]; bx,by=VERTS[SEGS[sid][1]]
    nx,ny=(by-ay),-(bx-ax)
    return (nx*math.cos(ang)+ny*math.sin(ang))*coef

@dataclass
class Run:
    sid:int; a0:float; a1:float; ir0:float; ir1:float

def refine_switch(px,py,a_lo,a_hi,left_sid,iters=10):
    for _ in range(iters):
        m=(a_lo+a_hi)/2
        sid,_=nearest_solid(px,py,m*TAU)
        if sid==left_sid: a_lo=m
        else: a_hi=m
    return (a_lo+a_hi)/2

def bake_point(px,py,bins=ANGLE_BINS):
    ids=[]; ds=[]
    for i in range(bins):
        a=(i+0.5)/bins*TAU; sid,t=nearest_solid(px,py,a); ids.append(sid); ds.append(t)
    bounds=[]
    for i in range(bins):
        if ids[i]!=ids[i-1]:
            lo=(i-0.5)/bins; hi=(i+0.5)/bins
            if i==0: lo=-0.5/bins; hi=0.5/bins
            bounds.append((i,refine_switch(px,py,lo,hi,ids[i-1])))
    runs=[]
    if not bounds:
        sid=ids[0]; t=ds[0] or 1e9; runs=[Run(sid,0.0,1.0,1/t,1/t)]
    else:
        for bi,(idx,a0) in enumerate(bounds):
            nxt_idx,a1=bounds[(bi+1)%len(bounds)]
            if bi==len(bounds)-1: a1+=1.0
            sid=ids[idx]
            eps=1e-7
            t0=ray_seg(px,py,(a0+eps)*TAU,sid) if sid>=0 else None
            t1=ray_seg(px,py,(a1-eps)*TAU,sid) if sid>=0 else None
            coef=line_coef(px,py,sid) if sid>=0 else 0.0
            runs.append(Run(sid,a0,a1,coef,coef))
    seq=[r.sid for r in runs]
    if seq:
        rots=[tuple(seq[i:]+seq[:i]) for i in range(len(seq))]; k=min(range(len(rots)),key=lambda i:rots[i]); runs=runs[k:]+runs[:k]
    ports={}
    for psid in PORTALS:
        visible=[]; ranges=[]
        for i in range(bins):
            a=(i+0.5)/bins*TAU; _,st=nearest_solid(px,py,a); t=portal_hit(px,py,a,psid,st); visible.append(t is not None); ranges.append(t)
        intervals=[]; start=None
        for i,v in enumerate(visible+[visible[0]]):
            if i<bins and v and start is None: start=i
            if start is not None and (i==bins or not v):
                end=i
                a0=(start+0.5)/bins; a1=(end-0.5)/bins
                t0=ranges[start]; t1=ranges[(end-1)%bins]
                coef=line_coef(px,py,psid)
                intervals.append(Run(psid,a0,a1,coef,coef)); start=None
        if visible[0] and visible[-1] and len(intervals)>=2:
            first,last=intervals[0],intervals[-1]; intervals=[Run(psid,last.a0,first.a1+1,last.ir0,last.ir0)]+intervals[1:-1]
        ports[psid]=intervals
    return {'runs':runs,'ports':ports}

def sig(p): return tuple(r.sid for r in p['runs'])

def walkable(x,y):
    return (20<=x<=76 and 20<=y<=76) or (74<=x<=116 and 40<=y<=60) or (112<=x<=172 and 20<=y<=78)

def build_field(spacing=GRID,bins=ANGLE_BINS):
    xs=[float(x) for x in range(0,193,int(spacing))]; ys=[float(y) for y in range(0,97,int(spacing))]
    pts={}; total=len(xs)*len(ys); n=0
    for y in ys:
        for x in xs:
            pts[(x,y)]=bake_point(x,y,bins); n+=1
            if n%10==0: print(f'  bake {n}/{total}',file=sys.stderr)
    return {'spacing':spacing,'xs':xs,'ys':ys,'pts':pts,'bins':bins}

def cell_for(field,x,y):
    s=field['spacing']; x0=max(field['xs'][0],min(field['xs'][-2],math.floor(x/s)*s)); y0=max(field['ys'][0],min(field['ys'][-2],math.floor(y/s)*s))
    return x0,y0,min(1,max(0,(x-x0)/s)),min(1,max(0,(y-y0)/s))

def ang_near(v,ref):
    while v-ref>0.5:v-=1
    while v-ref<-0.5:v+=1
    return v

def tri(v00,v10,v01,v11,fx,fy,circular=False):
    if circular:
        ref=v00; v10=ang_near(v10,ref); v01=ang_near(v01,ref); v11=ang_near(v11,ref)
    if fx+fy<=1: v=v00+fx*(v10-v00)+fy*(v01-v00)
    else: v=v11+(1-fx)*(v01-v11)+(1-fy)*(v10-v11)
    return wrapturn(v) if circular else v

def runtime_runs(field,x,y):
    x0,y0,fx,fy=cell_for(field,x,y); s=field['spacing']
    ps=[field['pts'][(x0,y0)],field['pts'][(x0+s,y0)],field['pts'][(x0,y0+s)],field['pts'][(x0+s,y0+s)]]
    idx=(1 if fx>=.5 else 0)+(2 if fy>=.5 else 0)
    def interp_layer(lists):
        maps=[]
        for ls in lists:
            m={}
            for r in ls: m.setdefault(r.sid,[]).append(r)
            maps.append(m)
        out=[]; all_sids=sorted(set().union(*(m.keys() for m in maps))); matched=0; boundary=0
        for sid in all_sids:
            rr=[m.get(sid,[]) for m in maps]
            if all(len(q)==1 for q in rr):
                rs=[q[0] for q in rr]; matched+=1
                out.append(Run(sid,tri(rs[0].a0,rs[1].a0,rs[2].a0,rs[3].a0,fx,fy,True),tri(rs[0].a1,rs[1].a1,rs[2].a1,rs[3].a1,fx,fy,True),tri(rs[0].ir0,rs[1].ir0,rs[2].ir0,rs[3].ir0,fx,fy),0.0))
            else:
                boundary+=1
                out.extend(rr[idx])
        return out,matched,boundary
    runs,matched,boundary=interp_layer([p['runs'] for p in ps])
    ports={}; pm=pb=0
    for sid in PORTALS:
        oo,m,b=interp_layer([p['ports'][sid] for p in ps]); ports[sid]=oo; pm+=m;pb+=b
    return runs,ports,(boundary+pb)==0,(x0,y0),matched+pm,boundary+pb

def shade(inv,bias):
    s=2 if inv>=82 else 1 if inv>=46 else 0; return max(0,min(2,s+bias))
PAL=[(8,10,16),(18,22,30),(30,37,50),(62,69,83),(104,111,128),(170,178,194)]

def profile_band(inv,prof):
    half=inv/2
    if prof==FULL:return HORIZON-half,HORIZON+half
    if prof==RAISED:return HORIZON-half,HORIZON+half*.75
    if prof==LINTEL:return HORIZON-half,HORIZON-half*.5
    return HORIZON+half*.75,HORIZON+half

def render_reference(x,y,yaw):
    pix=[[(12,14,22) if yy<HORIZON else (24,27,35) for _ in range(W)] for yy in range(H)]
    yr=yaw/256*TAU
    for sx in range(W):
        rel=math.atan((sx-W/2)/(W/2)); a=yr+rel; sid,t=nearest_solid(x,y,a)
        solidt=t
        if sid>=0 and t:
            z=max(.01,t*math.cos(rel)); inv=min(255,2560/z); top,bot=profile_band(inv,SEGS[sid][3]); col=PAL[3+shade(inv,SEGS[sid][2])]
            for yy in range(max(0,int(top)),min(H,int(bot)+1)):pix[yy][sx]=col
        for psid in PORTALS:
            pt=portal_hit(x,y,a,psid,solidt)
            if pt:
                z=max(.01,pt*math.cos(rel)); inv=min(255,2560/z); top,bot=profile_band(inv,SEGS[psid][3]); col=PAL[3+shade(inv,SEGS[psid][2])]
                for yy in range(max(0,int(top)),min(H,int(bot)+1)):pix[yy][sx]=col
    return pix

def run_overlap(run,yawturn):
    half=.125; center=yawturn
    a0,a1=run.a0,run.a1
    if a1<a0:a1+=1
    k=round(center-((a0+a1)/2)); a0+=k;a1+=k
    lo=max(a0,center-half); hi=min(a1,center+half)
    return None if hi<=lo else (lo,hi,a0,a1)

def render_baked(field,x,y,yaw):
    pix=[[(12,14,22) if yy<HORIZON else (24,27,35) for _ in range(W)] for yy in range(H)]; depth=[1e9]*W
    runs,ports,safe,cell,matched,boundary=runtime_runs(field,x,y); yt=yaw/256
    def draw_run(r,portal=False):
        ov=run_overlap(r,yt)
        if not ov:return
        lo,hi,a0,a1=ov
        def xt(a): return W/2+(W/2)*math.tan((a-yt)*TAU)
        xlo=max(0,int(math.floor(xt(lo)))); xhi=min(W-1,int(math.ceil(xt(hi))))
        denom=(a1-a0) if abs(a1-a0)>1e-9 else 1
        for sx in range(xlo,xhi+1):
            rel=math.atan((sx-W/2)/(W/2)); aw=yt+rel/TAU; aw=ang_near(aw,(a0+a1)/2); q=max(0,min(1,(aw-a0)/denom)); ir=invrad_from_coef(r.sid,r.ir0,aw*TAU)
            if ir<=0:continue
            radial=1/ir; z=max(.01,radial*math.cos(rel)); inv=min(255,2560/z)
            if portal and z>=depth[sx]: continue
            if not portal: depth[sx]=z
            top,bot=profile_band(inv,SEGS[r.sid][3]); col=PAL[3+shade(inv,SEGS[r.sid][2])]
            for yy in range(max(0,int(top)),min(H,int(bot)+1)):pix[yy][sx]=col
    for r in runs: draw_run(r,False)
    for sid in PORTALS:
        for r in ports[sid]: draw_run(r,True)
    return pix,safe,cell,len(runs)+sum(len(v) for v in ports.values()),matched,boundary

def mismatch(a,b):
    n=0
    for y in range(H):
        for x in range(W): n+=a[y][x]!=b[y][x]
    return n/(W*H)

@dataclass
class State:
    xq:int=32<<4; yq:int=48<<4; yaw:int=0; speed:int=0; strafe:int=0; turn:int=0; scale:int=1; manual:int=0; phase:int=0; ticks:int=0

def slew(c,t,s): return min(t,c+s) if c<t else max(t,c-s) if c>t else c
def sm(v,s): return v*s
def walkq(xq,yq): return walkable(xq>>4,yq>>4)
def step_demo(st):
    frames,target,thr=DEMO[st.phase]; target_speed=thr*192; st.speed=slew(st.speed,target_speed,6); st.strafe=slew(st.strafe,0,6)
    e=((target-st.yaw+128)&255)-128; desired=max(-40,min(40,e<<2)); st.turn=slew(st.turn,desired,4); ys=st.turn*st.scale
    st.yaw=(st.yaw + ((ys+8)>>4 if ys>=0 else -(((-ys)+8)>>4)))&255
    sn=SIN[st.yaw]; cs=SIN[(st.yaw+64)&255]; dx=((st.speed*cs)>>11)*st.scale; dy=((st.speed*sn)>>11)*st.scale
    if walkq(st.xq+dx,st.yq):st.xq+=dx
    if walkq(st.xq,st.yq+dy):st.yq+=dy
    st.ticks+=st.scale
    if st.ticks>=frames:
        st.ticks=0
        if st.phase+1<len(DEMO):st.phase+=1

def ascii_view(pix,w=40,h=18):
    chars=' .:-=+*#%@'; out=[]
    for yy in range(h):
        row=''
        for xx in range(w):
            c=pix[int(yy*H/h)][int(xx*W/w)]; lum=sum(c)/3; row+=chars[min(len(chars)-1,int(lum/256*len(chars)))]
        out.append(row)
    return out

def interactive(field):
    def app(scr):
        curses.curs_set(0); scr.nodelay(True); st=State(); auto=False
        while True:
            if auto: step_demo(st)
            x,y=st.xq/16,st.yq/16; ref=render_reference(x,y,st.yaw); bak,safe,cell,nr,matched,boundary=render_baked(field,x,y,st.yaw); err=mismatch(ref,bak)
            scr.erase(); scr.addstr(0,0,'POLAR VISIBILITY FIELD  |  W/S move  A/D turn  Q/E strafe-ish  P auto  R reset  X exit')
            scr.addstr(1,0,f'x={x:6.2f} y={y:6.2f} yaw={st.yaw:3d} cell={cell} topology={"SAFE" if safe else "BOUNDARY"} runs={nr} match/boundary={matched}/{boundary} mismatch={err*100:5.2f}%')
            av=ascii_view(bak); rv=ascii_view(ref)
            scr.addstr(3,0,'BAKED'); scr.addstr(3,44,'REFERENCE')
            for i,(aa,rr) in enumerate(zip(av,rv)):
                try: scr.addstr(4+i,0,aa); scr.addstr(4+i,44,rr)
                except curses.error: pass
            scr.refresh(); ch=scr.getch()
            if ch in (ord('x'),ord('X')):break
            if ch in (ord('r'),ord('R')):st=State()
            if ch in (ord('p'),ord('P')):auto=not auto
            ang=st.yaw/256*TAU; mv=1.0
            if ch in (ord('a'),ord('A')):st.yaw=(st.yaw-2)&255
            if ch in (ord('d'),ord('D')):st.yaw=(st.yaw+2)&255
            dx=dy=0
            if ch in (ord('w'),ord('W')):dx,dy=math.cos(ang)*mv,math.sin(ang)*mv
            if ch in (ord('s'),ord('S')):dx,dy=-math.cos(ang)*mv,-math.sin(ang)*mv
            if ch in (ord('q'),ord('Q')):dx,dy=math.cos(ang-math.pi/2)*mv,math.sin(ang-math.pi/2)*mv
            if ch in (ord('e'),ord('E')):dx,dy=math.cos(ang+math.pi/2)*mv,math.sin(ang+math.pi/2)*mv
            nx,ny=x+dx,y+dy
            if walkable(nx,y):st.xq=int(nx*16)
            if walkable(st.xq/16,ny):st.yq=int(ny*16)
            time.sleep(.03)
    curses.wrapper(app)

def save_ppm(path,pix,scale=2):
    with open(path,'wb') as f:
        f.write(f'P6\n{W*scale} {H*scale}\n255\n'.encode())
        for row in pix:
            rr=b''.join(bytes(c)*scale for c in row)
            for _ in range(scale):f.write(rr)

def report(field):
    safe=unsafe=0
    s=field['spacing']; errs=[]; runs=[]
    for y0 in field['ys'][:-1]:
        for x0 in field['xs'][:-1]:
            if not walkable(x0+s/2,y0+s/2):continue
            x,y=x0+s/2,y0+s/2; _,_,ok,_,_,_=runtime_runs(field,x,y); safe+=ok; unsafe+=not ok
            for yaw in (0,32,64,96,128,160,192,224):
                a=render_reference(x,y,yaw); b,_,_,nr,_,_=render_baked(field,x,y,yaw); errs.append(mismatch(a,b)); runs.append(nr)
    print('=== POLAR FIELD REPORT ===')
    print(f'grid spacing: {s:g} world units; angular discovery bins: {field["bins"]}')
    print(f'bake points: {len(field["pts"])}')
    print(f'walkable test cells: {safe+unsafe}; topology-safe: {safe} ({100*safe/max(1,safe+unsafe):.1f}%); boundary: {unsafe}')
    print(f'mean image mismatch at cell centers / 8 yaws: {100*sum(errs)/max(1,len(errs)):.2f}% ; max {100*max(errs,default=0):.2f}%')
    print(f'mean reconstructed visible runs: {sum(runs)/max(1,len(runs)):.2f}')
    print('NOTE: unsafe cells deliberately use nearest baked topology in v1. They are the adaptive-refinement/selector targets.')

def record(field,out,frames=300,fps=30):
    try:
        from PIL import Image,ImageDraw,ImageFont
    except Exception as e:
        raise SystemExit('Recording requires Pillow. On Termux: pkg install python-pillow ffmpeg') from e
    ff=shutil.which('ffmpeg')
    if not ff: raise SystemExit('ffmpeg not found')
    tmp=os.path.join('/tmp','polarfield_frames'); shutil.rmtree(tmp,ignore_errors=True); os.makedirs(tmp)
    st=State(); font=ImageFont.load_default(); totalerr=0
    for i in range(frames):
        step_demo(st); x,y=st.xq/16,st.yq/16; ref=render_reference(x,y,st.yaw); bak,safe,cell,nr,matched,boundary=render_baked(field,x,y,st.yaw); err=mismatch(ref,bak); totalerr+=err
        canvas=Image.new('RGB',(960,576),(10,12,18)); d=ImageDraw.Draw(canvas)
        d.rectangle((0,0,319,575),fill=(16,18,25)); sx=1.55; ox=12; oy=150
        for gx in field['xs']: d.line((ox+gx*sx,oy,ox+gx*sx,oy+96*sx),fill=(35,38,50))
        for gy in field['ys']: d.line((ox,oy+gy*sx,ox+192*sx,oy+gy*sx),fill=(35,38,50))
        x0,y0=cell; s=field['spacing']; d.rectangle((ox+x0*sx,oy+y0*sx,ox+(x0+s)*sx,oy+(y0+s)*sx),outline=(70,220,120) if safe else (240,90,70),width=3)
        for sid in SOLIDS:
            a,b=SEGS[sid][0],SEGS[sid][1]; p0,p1=VERTS[a],VERTS[b]; d.line((ox+p0[0]*sx,oy+p0[1]*sx,ox+p1[0]*sx,oy+p1[1]*sx),fill=(220,220,225),width=3)
        for sid in PORTALS:
            a,b=SEGS[sid][0],SEGS[sid][1]; p0,p1=VERTS[a],VERTS[b]; d.line((ox+p0[0]*sx,oy+p0[1]*sx,ox+p1[0]*sx,oy+p1[1]*sx),fill=(80,170,245),width=2)
        cx,cy=ox+x*sx,oy+y*sx; ya=st.yaw/256*TAU; d.ellipse((cx-5,cy-5,cx+5,cy+5),fill=(255,205,70)); d.line((cx,cy,cx+35*math.cos(ya),cy+35*math.sin(ya)),fill=(255,205,70),width=3)
        def put(p,xy):
            im=Image.new('RGB',(W,H)); im.putdata([c for row in p for c in row]); im=im.resize((320,288),Image.Resampling.NEAREST); canvas.paste(im,xy)
        put(ref,(320,0)); put(bak,(320,288))
        diff=[]
        for yy in range(H):
            for xx in range(W): diff.append((245,70,80) if ref[yy][xx]!=bak[yy][xx] else tuple(v//4 for v in ref[yy][xx]))
        di=Image.new('RGB',(W,H)); di.putdata(diff); di=di.resize((320,288),Image.Resampling.NEAREST); canvas.paste(di,(640,0))
        d.rectangle((640,288,959,575),fill=(18,20,28)); lines=['ADAPTIVE POLAR FIELD POC','top: exact ray reference','middle-bottom: prebaked field','right-top: pixel difference',f'pos {x:.1f},{y:.1f}',f'yaw8 {st.yaw}',f'cell {cell}',f'topology {"SAFE" if safe else "BOUNDARY"}',f'visible runs {nr}',f'matched/boundary {matched}/{boundary}',f'pixel mismatch {err*100:.2f}%',f'grid spacing {field["spacing"]:g}',f'angular bins {field["bins"]}']
        yy=310
        for line in lines:d.text((660,yy),line,font=font,fill=(235,235,240));yy+=20
        canvas.save(os.path.join(tmp,f'{i:05d}.png'))
        if i%30==0: print(f'  frame {i}/{frames}',file=sys.stderr)
    subprocess.run([ff,'-y','-framerate',str(fps),'-i',os.path.join(tmp,'%05d.png'),'-c:v','libx264','-pix_fmt','yuv420p','-crf','20',out],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    shutil.rmtree(tmp,ignore_errors=True); print(f'wrote {out}; mean mismatch {100*totalerr/frames:.2f}%')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--spacing',type=int,default=16);ap.add_argument('--bins',type=int,default=2048);ap.add_argument('--report',action='store_true');ap.add_argument('--interactive',action='store_true');ap.add_argument('--record');ap.add_argument('--frames',type=int,default=300);ap.add_argument('--fps',type=int,default=30)
    a=ap.parse_args(); print('Baking spatial polar visibility field...',file=sys.stderr); field=build_field(a.spacing,a.bins)
    if a.report or (not a.interactive and not a.record):report(field)
    if a.record:record(field,a.record,a.frames,a.fps)
    if a.interactive:interactive(field)
if __name__=='__main__':main()
