#!/usr/bin/env python3
from pathlib import Path
import re

p=Path('src/tilesector_core.c')
s=p.read_text()
if 'k_vertex_rot_raw_64' in s:
    print('Stage 10 vertex rotation LUT already applied')
    raise SystemExit(0)

# Parse the authored world and exact runtime sine table so this experiment stays
# mechanically coupled to the renderer instead of duplicating hand-maintained data.
vm=re.search(r'static const TSVertex k_vertices\[\] = \{(.*?)\n\};',s,re.S)
sm=re.search(r'static const int8_t k_sin\[256\] = \{(.*?)\n\};',s,re.S)
if not vm or not sm:
    raise SystemExit('failed to locate vertex/sine tables')
verts=[tuple(map(int,m)) for m in re.findall(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}',vm.group(1))]
sins=[int(x) for x in re.findall(r'-?\d+',sm.group(1))]
if len(verts)!=14 or len(sins)!=256:
    raise SystemExit(f'unexpected table sizes vertices={len(verts)} sin={len(sins)}')

def sra3(v):
    # Z80/SDCC arithmetic signed >> 3 semantics.
    return v >> 3

rows=[]
for yaw in range(64):
    sn=sins[yaw]
    cs=sins[(yaw+64)&255]
    vals=[]
    for vx,vy in verts:
        # Store camera-X then camera-Z in Q4 for quadrant zero. The other three
        # quadrants are exact sign/swap identities, so 64 rows cover all 256 yaw.
        xraw=-vx*sn + vy*cs
        zraw=vx*cs + vy*sn
        vals.extend((xraw,zraw))
    rows.append(vals)

lines=[]
lines.append('/* Stage 10: raw Q7-ish rotated world numerators for the first quarter-turn.\n'
             ' * Keep the low three bits until after player subtraction: rounding\n'
             ' * world and player independently can swallow a legal 1/16-unit\n'
             ' * camera step at integer-world boundaries. Same 3584-byte LUT,\n'
             ' * still replacing 56 signed multiplies per rendered frame. */')
lines.append('static const int16_t k_vertex_rot_raw_64[64][TS_VERTICES][2] = {')
for vals in rows:
    pairs=[f'{{{vals[i]},{vals[i+1]}}}' for i in range(0,len(vals),2)]
    lines.append('    {'+', '.join(pairs)+'},')
lines.append('};\n')
table='\n'.join(lines)

anchor='/* ----- Q4 camera/projection ----- */\n'
if anchor not in s:
    raise SystemExit('projection anchor not found')
s=s.replace(anchor,anchor+'\n'+table+'\n',1)

old=re.search(r'static void transform_vertices_q4\(const TSState \*s\) \{.*?\n\}\n\nstatic uint8_t inv_for_zq4',s,re.S)
if not old:
    raise SystemExit('transform function not found')
new=r'''static void transform_vertices_q4(const TSState *s) {
    uint8_t vi;
    uint8_t a=(uint8_t)(s->yaw & 63u);
    uint8_t q=(uint8_t)(s->yaw >> 6);
    int16_t px_i=(int16_t)(s->x_q4 >> 4);
    int16_t py_i=(int16_t)(s->y_q4 >> 4);
    uint8_t fx=(uint8_t)(s->x_q4 & 15);
    uint8_t fy=(uint8_t)(s->y_q4 & 15);
    int8_t sn=k_sin[s->yaw];
    int8_t cs=k_sin[(uint8_t)(s->yaw+64u)];
    int16_t frac_z=(int16_t)((int16_t)fx*cs+(int16_t)fy*sn);
    int16_t frac_x=(int16_t)(-(int16_t)fx*sn+(int16_t)fy*cs);
    int16_t frac_z_q4=round_shift7(frac_z);
    int16_t frac_x_q4=round_shift7(frac_x);
    int16_t player_z_raw=(int16_t)((int16_t)px_i*cs+(int16_t)py_i*sn);
    int16_t player_x_raw=(int16_t)(-(int16_t)px_i*sn+(int16_t)py_i*cs);
    const int16_t *r=&k_vertex_rot_raw_64[a][0][0];

    for(vi=0u;vi<TS_VERTICES;++vi) {
        int16_t bx=*r++;
        int16_t bz=*r++;
        int16_t rx,rz;
        if(q==0u)      { rx=bx;  rz=bz;  }
        else if(q==1u) { rx=-bz; rz=bx;  }
        else if(q==2u) { rx=-bx; rz=-bz; }
        else           { rx=bz;  rz=-bx; }
        g_cam_x_q4[vi]=(int16_t)(((rx-player_x_raw)>>3)-frac_x_q4);
        g_cam_z_q4[vi]=(int16_t)(((rz-player_z_raw)>>3)-frac_z_q4);
    }
}

static uint8_t inv_for_zq4'''
s=s[:old.start()]+new+s[old.end():]
p.write_text(s)
print('Applied Stage 10 quarter-turn pre-rotated vertex LUT.')
