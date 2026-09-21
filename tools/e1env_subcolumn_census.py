#!/usr/bin/env python3
from __future__ import annotations
import argparse,csv,re,statistics
from pathlib import Path
from collections import Counter,defaultdict

NUM_RE=re.compile(r'-?0x[0-9A-Fa-f]+|-?\d+')

def nums(s): return [int(x,0) for x in NUM_RE.findall(s)]
def arr(text,name):
    m=re.search(r'static\s+const\s+[^;=]+?\b'+re.escape(name)+r'\s*\[[^\]]+\]\s*=\s*\{(.*?)\};',text,re.S)
    if not m: raise RuntimeError('missing '+name)
    return nums(m.group(1))
def signed_q12(v):
    v &= 4095
    return v-4096 if v>=2048 else v

def parse_header(path):
    t=Path(path).read_text()
    def macro(n):
        m=re.search(r'#define\s+'+re.escape(n)+r'\s+(-?\d+)u?',t)
        if not m: raise RuntimeError('missing macro '+n)
        return int(m.group(1))
    return {k:macro(k) for k in ['E1ENV_WORLD_MIN_X','E1ENV_WORLD_MIN_Y','E1ENV_COLS','E1ENV_ROWS','E1ENV_PROGRAM_COUNT']}

def parse_idx(gen):
    banks=[]
    for p in sorted(Path(gen).glob('e1env_idx_*.c'), key=lambda p:int(p.stem.split('_')[-1])):
        t=p.read_text(); idx=arr(t,'k_idx'); d=arr(t,'k_dict'); banks.append((idx,d))
    return banks

def parse_programs(gen):
    progs=[]
    for p in sorted(Path(gen).glob('e1env_prog_*.c')):
        t=p.read_text(); offs=arr(t,'k_off'); stream=arr(t,'k_stream')
        for off in offs:
            n=stream[off]
            progs.append([(stream[off+1+2*i],stream[off+2+2*i]) for i in range(n)])
    return progs

def parse_center_lut(path):
    t=Path(path).read_text()
    try: body=t.split('_g_e1env_center_col_lut::',1)[1]
    except IndexError: raise RuntimeError('missing center LUT symbol')
    vals=[]
    for line in body.splitlines():
        if '.db' in line: vals += nums(line.split('.db',1)[1])
    if len(vals)!=1025: raise RuntimeError(f'center LUT expected 1025, got {len(vals)}')
    return vals

def ratio_q8(n,d,recip):
    if not d: return 0
    rec=recip[d]
    p_lo=n*(rec&255); p_hi=n*((rec>>8)&255)
    q=p_hi+((p_lo+128)>>8)
    return min(255,q)

def bearing_q12(dx,dy,recip,atan):
    if dx==0 and dy==0: return 0
    sx=dx<0; sy=dy<0; ax=abs(dx); ay=abs(dy)
    while ax>255 or ay>255:
        ax=(ax+1)>>1; ay=(ay+1)>>1
    if ax>=ay: a=atan[ratio_q8(ay,ax,recip)]
    else: a=1024-atan[ratio_q8(ax,ay,recip)]
    if sx: a=2048-a
    if sy: a=-a
    return a&4095

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--generated-dir',required=True)
    ap.add_argument('--trace-csv',required=True)
    ap.add_argument('--center-lut')
    a=ap.parse_args()
    gen=Path(a.generated_dir)
    h=parse_header(gen/'e1env_generated.h')
    idxbanks=parse_idx(gen); progs=parse_programs(gen)
    mt=(gen/'optimized_renderer_map_data.inc').read_text()
    vx=arr(mt,'k_tspf_vx'); vy=arr(mt,'k_tspf_vy')
    recip=arr(mt,'k_tspf_recip8_q16'); atan=arr(mt,'k_tspf_atan_q12'); angle_x_pos=arr(mt,'k_tspf_angle_x_pos')
    centers=[-497,-461,-422,-378,-330,-279,-223,-163,-101,-36,29,94,156,216,273,325,373,417,457,494]
    import bisect
    def center_code(rel):
        c=bisect.bisect_left(centers,rel)
        aa=abs(rel); x=(160-angle_x_pos[aa]) if rel<0 else angle_x_pos[aa]; x=min(159,x)
        dx=max(-4,min(3,x-8*c))
        return c+((dx+4)<<5)
    lut=[center_code(rel) for rel in range(-512,513)]
    if a.center_lut:
        actual=parse_center_lut(a.center_lut)
        if actual != lut:
            bad=[i for i,(x,y) in enumerate(zip(actual,lut)) if x!=y]
            raise RuntimeError(f'center LUT reconstruction mismatch count={len(bad)} first_rel={bad[0]-512 if bad else None}')
    assert len(progs)==h['E1ENV_PROGRAM_COUNT']

    xs=[]
    for code in lut:
        c=code&31; dx=(code>>5)-4; xs.append(c*8+dx)
    regress=sum(b<a for a,b in zip(xs,xs[1:]))
    leaps=max((b-a for a,b in zip(xs,xs[1:])), default=0)

    total_visible=total_internal=total_drop=total_sub8=0
    width_err_abs=[]; samecell_widths=[]; per_sid_prev={}
    accordion=wrong_dir=coarse_jump_smooth=0
    event_samples=[]
    sid_history=defaultdict(list)
    sid_drop=Counter(); sid_accordion=Counter()

    def fetch_prog(xq,yq):
        rx=xq-(h['E1ENV_WORLD_MIN_X']<<4); ry=yq-(h['E1ENV_WORLD_MIN_Y']<<4)
        if rx<0 or ry<0 or rx>=h['E1ENV_COLS'] or ry>=h['E1ENV_ROWS']: return None
        gx=rx; gy=ry; local=((gy&7)*h['E1ENV_COLS'])+gx; band=gy>>3
        if band>=len(idxbanks): return None
        ix,d=idxbanks[band]; di=ix[local]
        if di>=len(d): return None
        pid=d[di]
        if pid>=len(progs): return None
        return progs[pid]

    with open(a.trace_csv,newline='') as f:
        rows=list(csv.DictReader(f))
    for fi,row in enumerate(rows):
        xq=int(row['x_q4']); yq=int(row['y_q4']); yaw=int(row['yaw']); yawq=yaw<<4
        prog=fetch_prog(xq,yq)
        if not prog: continue
        cur={}
        n=len(prog)
        bcache={}
        for vid,_ in prog:
            bcache[vid]=bearing_q12((vx[vid]<<4)-xq,(vy[vid]<<4)-yq,recip,atan)
        for i,(v0,owner) in enumerate(prog):
            if owner==0xff: continue
            v1=prog[(i+1)%n][0]; sid=owner&31
            if v1 not in bcache:
                bcache[v1]=bearing_q12((vx[v1]<<4)-xq,(vy[v1]<<4)-yq,recip,atan)
            a0=bcache[v0]; a1=bcache[v1]
            length=(a1-a0)&4095
            if not length or length>=2048: continue
            st=signed_q12(a0-yawq); en=st+length
            while en < -512: st+=4096; en+=4096
            while st > 512: st-=4096; en-=4096
            lo=max(st,-512); hi=min(en,512)
            if hi<=lo: continue
            total_visible+=1
            internal=(lo==st and hi==en)
            if not internal: continue
            total_internal+=1
            code0=lut[lo+512]; code1=lut[hi+512]
            c0=code0&31; c1=code1&31
            x0=c0*8+((code0>>5)-4); x1=c1*8+((code1>>5)-4)
            pw=max(0,x1-x0); cw=max(0,8*(c1-c0))
            width_err_abs.append(abs(cw-pw))
            if 0<pw<8: total_sub8+=1
            if pw>0 and cw==0:
                total_drop+=1; samecell_widths.append(pw); sid_drop[sid]+=1
            cur[sid]=(pw,cw,v0,v1,lo,hi)
            sid_history[sid].append((fi,pw,cw))
            prev=per_sid_prev.get(sid)
            if prev:
                pp,pc,*_=prev
                dp=pw-pp; dc=cw-pc
                if abs(dp)<=2 and abs(dc)>=8:
                    coarse_jump_smooth+=1; sid_accordion[sid]+=1
                    if len(event_samples)<30: event_samples.append((fi,sid,pp,pw,pc,cw,dp,dc))
                if dp and dc and ((dp>0)!=(dc>0)):
                    wrong_dir+=1
                    if abs(dc)>=8: accordion+=1
            per_sid_prev[sid]=cur[sid]
        for sid in list(per_sid_prev):
            if sid not in cur: per_sid_prev.pop(sid,None)

    reversals=[]
    for sid,hist in sid_history.items():
        for aa,bb,cc in zip(hist,hist[1:],hist[2:]):
            if bb[0]!=aa[0]+1 or cc[0]!=bb[0]+1: continue
            dc1=bb[2]-aa[2]; dc2=cc[2]-bb[2]
            if abs(dc1)>=8 and abs(dc2)>=8 and dc1*dc2<0 and abs(cc[1]-aa[1])<=2:
                reversals.append((sid,aa,bb,cc))

    def mean(v): return statistics.fmean(v) if v else 0
    def pct(v,q):
        if not v:return 0
        s=sorted(v); return s[int((len(s)-1)*q)]
    print('SUBCOL_CENSUS')
    print(f'frames={len(rows)} programs={len(progs)} visible_spans={total_visible} internal_spans={total_internal}')
    print(f'lut_regressions={regress} lut_max_step_px={leaps} reconstructed_x_min={min(xs)} max={max(xs)}')
    print(f'positive_sub8_spans={total_sub8} positive_width_dropped={total_drop} drop_share_internal={100*total_drop/max(1,total_internal):.2f}%')
    print(f'width_error_abs mean={mean(width_err_abs):.2f}px p50={pct(width_err_abs,.5)} p95={pct(width_err_abs,.95)} max={max(width_err_abs) if width_err_abs else 0}')
    print(f'temporal_coarse_jump_while_physical_delta_le2={coarse_jump_smooth} wrong_direction_events={wrong_dir} strong_opposite_8px_events={accordion}')
    print('dropped_width_hist='+','.join(f'{k}px:{v}' for k,v in sorted(Counter(samecell_widths).items())))
    print('top_drop_surfaces='+','.join(f'sid{k}:{v}' for k,v in sid_drop.most_common(12)))
    print('top_accordion_surfaces='+','.join(f'sid{k}:{v}' for k,v in sid_accordion.most_common(12)))
    print(f'three_frame_coarse_reversals={len(reversals)}')
    for sid,aa,bb,cc in reversals[:12]:
        print(f'reversal_sample sid={sid} frames={aa[0]},{bb[0]},{cc[0]} physical={aa[1]}->{bb[1]}->{cc[1]} coarse={aa[2]}->{bb[2]}->{cc[2]}')
    for e in event_samples[:20]:
        fi,sid,pp,pw,pc,cw,dp,dc=e
        print(f'accordion_sample frame={fi} sid={sid} physical={pp}->{pw} coarse={pc}->{cw} dphys={dp:+d} dcoarse={dc:+d}')

if __name__=='__main__': main()
