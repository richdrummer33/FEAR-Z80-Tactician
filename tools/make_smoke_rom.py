from pathlib import Path
import sys

ROM_SIZE=0x8000
HEADER=0x7ff0
rom=bytearray([0x00])*ROM_SIZE
code=bytearray()
patches=[]

def b(*xs): code.extend(xs)
def ld_a(n): b(0x3e,n&0xff)
def out(port): b(0xd3,port&0xff)
def ld_c(n): b(0x0e,n&0xff)
def ld_b(n): b(0x06,n&0xff)
def ld_hl_label(name):
    b(0x21,0,0); patches.append((len(code)-2,name))
def otir(): b(0xed,0xb3)
def vdp_reg(reg,val):
    ld_a(val); out(0xbf); ld_a(0x80|reg); out(0xbf)
def vdp_addr(addr, mode):
    # mode: 0x40 VRAM write, 0xC0 CRAM write
    ld_a(addr & 0xff); out(0xbf); ld_a(((addr>>8)&0x3f)|mode); out(0xbf)

# reset / CPU
b(0xf3)          # DI
b(0xed,0x56)     # IM 1
b(0x31,0xf0,0xdf)# LD SP,$DFF0

# VDP setup, display off while uploading
vdp_reg(1,0x80)
vdp_reg(0,0x06)
vdp_reg(2,0xf7) # name table $1800
vdp_reg(5,0xbf) # sprite table $1f00
vdp_reg(6,0xff)
vdp_reg(7,0x00)
vdp_reg(8,0x00)
vdp_reg(9,0x00)
vdp_reg(10,0xff)

# palette (GG CRAM is 16 x 12-bit little endian)
vdp_addr(0,0xc0)
ld_hl_label('palette'); ld_c(0xbe); ld_b(32); otir()

# tile patterns at VRAM $0000
vdp_addr(0,0x40)
ld_hl_label('tiles'); ld_c(0xbe); ld_b(6*32); otir()

# name table at VRAM $1800 (32 x 24 entries = 1536 bytes)
vdp_addr(0x1800,0x40)
ld_hl_label('nametable'); ld_c(0xbe)
for _ in range(6):
    ld_b(0); otir() # B=0 => 256 OUTI iterations

# display on, interrupts remain off for static smoke image
vdp_reg(1,0xc0)
# debugger canary: prove the emulator runner can inspect GG system RAM
ld_a(0x42); b(0x32,0x00,0xC0)  # LD ($C000),A
b(0x18,0xfe) # JR $ forever

# data follows code
labels={}
def align(n):
    while len(code)%n: b(0)
align(16)
labels['palette']=len(code)
colors=[
    (0,0,0),      # 0 black
    (1,2,4),      # 1 floor
    (7,8,9),      # 2 wall
    (15,9,1),     # 3 door
    (1,12,15),    # 4 blue
    (15,2,2),     # 5 red
    (15,15,15),   # 6 white
    (15,15,1),    # 7 yellow
    (0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0)
]
for r,g,bl in colors:
    v=(r&15)|((g&15)<<4)|((bl&15)<<8)
    code.extend((v&0xff,(v>>8)&0xff))

labels['tiles']=len(code)
# six solid 4bpp tiles. For each row, four bitplanes.
for color in range(6):
    for y in range(8):
        for plane in range(4):
            code.append(0xff if (color>>plane)&1 else 0x00)

labels['nametable']=len(code)
# 32 x 24. GG visible crop is x 6..25, y 3..20.
# Build a little CQB room directly in the crop.
for y in range(24):
    for x in range(32):
        tile=0
        if 6<=x<=25 and 3<=y<=20:
            vx=x-6; vy=y-3
            tile=1
            if vx in (0,19) or vy in (0,17): tile=2
            # interior steel spine, two doors, flank blocks
            if 2<=vy<=15 and vx==9: tile=2
            if vy in (5,12) and vx==9: tile=3
            if vy in (6,11) and 3<=vx<=6: tile=2
            if vy in (6,11) and 13<=vx<=16: tile=2
            if (vx,vy)==(2,2): tile=4
            if (vx,vy)==(17,15): tile=5
        code.extend((tile&0xff,0x00))

# patch absolute label addresses (ROM mapped from zero)
for pos,name in patches:
    addr=labels[name]
    code[pos]=addr&0xff; code[pos+1]=(addr>>8)&0xff

if len(code)>=HEADER:
    raise SystemExit(f'code too large: {len(code)}')
rom[:len(code)]=code

# Sega header, GBDK makebin-compatible GG export code 6, 32 KiB code 0xC
rom[HEADER:HEADER+10]=b'TMR SEGA  '
# product/version bytes left zero at 0x7ffc..0x7ffe
rom[HEADER+0x0f]=(6<<4)|0x0c
chk=sum(rom[:HEADER]) & 0xffff
rom[HEADER+0x0a]=chk&0xff; rom[HEADER+0x0b]=(chk>>8)&0xff

out=Path(sys.argv[1] if len(sys.argv) > 1 else 'build/gg_smoke.gg')
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(rom)
print(f'wrote {out} bytes={len(rom)} code+data={len(code)} checksum=0x{chk:04x}')
print('labels', {k:hex(v) for k,v in labels.items()})
