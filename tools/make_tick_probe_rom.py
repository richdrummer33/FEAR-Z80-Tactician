from pathlib import Path
import sys

ROM_SIZE = 0x8000
HEADER = 0x7FF0
MAIN = 0x0100
rom = bytearray([0x00]) * ROM_SIZE

class Asm:
    def __init__(self, base):
        self.base = base
        self.code = bytearray()
        self.labels = {}
        self.abs16 = []
        self.rel8 = []
    @property
    def pc(self): return self.base + len(self.code)
    def emit(self,*xs): self.code.extend(x & 0xff for x in xs)
    def label(self,name): self.labels[name]=self.pc
    def jp(self,name, cond=None):
        op = {None:0xC3,'nz':0xC2,'z':0xCA,'nc':0xD2,'c':0xDA}[cond]
        self.emit(op,0,0); self.abs16.append((len(self.code)-2,name))
    def call(self,name): self.emit(0xCD,0,0); self.abs16.append((len(self.code)-2,name))
    def jr(self,name, cond=None):
        op={None:0x18,'nz':0x20,'z':0x28,'nc':0x30,'c':0x38}[cond]
        self.emit(op,0); self.rel8.append((len(self.code)-1,name))
    def ld_hl_label(self,name):
        self.emit(0x21,0,0); self.abs16.append((len(self.code)-2,name))
    def patch(self):
        for pos,name in self.abs16:
            a=self.labels[name]
            self.code[pos]=a&0xff; self.code[pos+1]=(a>>8)&0xff
        for pos,name in self.rel8:
            src=self.base+pos+1
            d=self.labels[name]-src
            if not -128 <= d <= 127: raise ValueError((name,d))
            self.code[pos]=d & 0xff

def emit_vdp_reg(a,reg,val):
    a.emit(0x3E,val,0xD3,0xBF,0x3E,0x80|reg,0xD3,0xBF)

def emit_vdp_addr(a,addr,mode):
    a.emit(0x3E,addr&0xff,0xD3,0xBF,0x3E,((addr>>8)&0x3f)|mode,0xD3,0xBF)

# Reset vector and IRQ handler.
rom[0:3] = bytes((0xC3, MAIN & 0xff, MAIN >> 8))  # JP MAIN
irq = bytearray((
    0xF5,       # PUSH AF
    0xDB,0xBF,  # IN A,($BF) acknowledge VDP IRQ
    0xF1,       # POP AF
    0xFB,       # EI
    0xED,0x4D   # RETI
))
rom[0x38:0x38+len(irq)] = irq

a=Asm(MAIN)
a.label('main')
a.emit(0xF3)             # DI
a.emit(0xED,0x56)        # IM 1
a.emit(0x31,0xF0,0xDF)   # LD SP,$DFF0

# VDP init copied from known-good smoke ROM.
emit_vdp_reg(a,1,0x80)   # display off, IRQ off
emit_vdp_reg(a,0,0x06)
emit_vdp_reg(a,2,0xF7)   # name table $1800
emit_vdp_reg(a,5,0xBF)
emit_vdp_reg(a,6,0xFF)
emit_vdp_reg(a,7,0x00)
emit_vdp_reg(a,8,0x00)
emit_vdp_reg(a,9,0x00)
emit_vdp_reg(a,10,0xFF)

# palette
emit_vdp_addr(a,0,0xC0)
a.ld_hl_label('palette'); a.emit(0x0E,0xBE,0x06,32,0xED,0xB3)
# tiles
emit_vdp_addr(a,0,0x40)
a.ld_hl_label('tiles'); a.emit(0x0E,0xBE,0x06,6*32,0xED,0xB3)
# nametable, 1536 bytes = 6 * 256 via B=0 OTIR
emit_vdp_addr(a,0x1800,0x40)
a.ld_hl_label('nametable'); a.emit(0x0E,0xBE)
for _ in range(6): a.emit(0x06,0x00,0xED,0xB3)

# RAM state: tick, frame, blue path idx, red path idx, blue x/y, red x/y
a.emit(0xAF)  # XOR A
a.emit(0x32,0x00,0xC0) # tick=0
a.emit(0x32,0x01,0xC0) # frame=0
a.emit(0x32,0x02,0xC0) # bidx=0
a.emit(0x32,0x03,0xC0) # ridx=0
# initial positions
a.emit(0x3E,2, 0x32,0x04,0xC0, 0x32,0x05,0xC0) # blue x=2 y=2
a.emit(0x3E,17,0x32,0x06,0xC0)
a.emit(0x3E,15,0x32,0x07,0xC0)

# display on + VBlank interrupt enable
emit_vdp_reg(a,1,0xE0)
a.emit(0xFB) # EI

a.label('loop')
a.emit(0x76) # HALT until VBlank IRQ
# ++frame
a.emit(0x3A,0x01,0xC0, 0x3C, 0x32,0x01,0xC0)
a.emit(0xFE,8) # CP 8
a.jr('loop','nz')
# frame=0
a.emit(0xAF,0x32,0x01,0xC0)
a.call('tick')
a.jr('loop')

# tick(): restore old cells, advance each path, draw new cells, increment tick
a.label('tick')
# restore old blue floor: A=1, B=[C004], C=[C005]
a.emit(0x3E,1)
a.emit(0x47) # LD B,A temporarily overwritten below; tile lost! use D for tile via draw helper expects A, B,C and copies A to E first.
# load B from memory through A then restore tile before call
a.emit(0x3A,0x04,0xC0,0x47,0x3A,0x05,0xC0,0x4F,0x3E,1)
a.call('draw_xy')
# restore old red floor
a.emit(0x3A,0x06,0xC0,0x47,0x3A,0x07,0xC0,0x4F,0x3E,1)
a.call('draw_xy')

# advance blue index modulo 8
a.emit(0x3A,0x02,0xC0,0x3C,0xE6,0x07,0x32,0x02,0xC0)
# HL = blue_path + 2*idx: E=A,D=0, HL table, ADD HL,DE twice
# LD E,A; LD D,0
a.emit(0x5F,0x16,0x00)
a.ld_hl_label('blue_path'); a.emit(0x19,0x19) # ADD HL,DE twice
# x=(HL), y=(HL+1)
a.emit(0x7E,0x32,0x04,0xC0,0x23,0x7E,0x32,0x05,0xC0)
# draw blue tile 4
a.emit(0x3A,0x04,0xC0,0x47,0x3A,0x05,0xC0,0x4F,0x3E,4)
a.call('draw_xy')

# advance red index modulo 8
a.emit(0x3A,0x03,0xC0,0x3C,0xE6,0x07,0x32,0x03,0xC0)
a.emit(0x5F,0x16,0x00)
a.ld_hl_label('red_path'); a.emit(0x19,0x19)
a.emit(0x7E,0x32,0x06,0xC0,0x23,0x7E,0x32,0x07,0xC0)
a.emit(0x3A,0x06,0xC0,0x47,0x3A,0x07,0xC0,0x4F,0x3E,5)
a.call('draw_xy')

# ++tick
a.emit(0x3A,0x00,0xC0,0x3C,0x32,0x00,0xC0)
a.emit(0xC9) # RET

# draw_xy(A=tile, B=x, C=y)
# Compute GG name-table byte address $1800 + (y+3)*64 + (x+6)*2.
a.label('draw_xy')
a.emit(0x5F)            # LD E,A (save tile)
a.emit(0x79)            # LD A,C
a.emit(0xC6,3)          # ADD A,3
a.emit(0x6F)            # LD L,A
a.emit(0x26,0)          # LD H,0
for _ in range(6): a.emit(0x29) # ADD HL,HL * 6
a.emit(0x78)            # LD A,B
a.emit(0x87)            # ADD A,A
a.emit(0xC6,12)         # + (6*2)
a.emit(0x4F)            # LD C,A
a.emit(0x06,0)          # LD B,0
a.emit(0x09)            # ADD HL,BC
a.emit(0x01,0x00,0x18)  # LD BC,$1800
a.emit(0x09)            # ADD HL,BC
# Set VDP write address, preserving tile in E
a.emit(0x7D,0xD3,0xBF)  # LD A,L / OUT BF
a.emit(0x7C,0xF6,0x40,0xD3,0xBF) # LD A,H / OR 40 / OUT BF
a.emit(0x7B,0xD3,0xBE)  # tile low byte
a.emit(0xAF,0xD3,0xBE)  # attr=0
a.emit(0xC9)

# Align data to 16 bytes.
while a.pc % 16: a.emit(0)
a.label('palette')
colors=[
    (0,0,0),(1,2,4),(7,8,9),(15,9,1),(1,12,15),(15,2,2),
    (15,15,15),(15,15,1),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0),(0,0,0)
]
for r,g,bv in colors:
    v=(r&15)|((g&15)<<4)|((bv&15)<<8)
    a.emit(v&0xff,(v>>8)&0xff)

a.label('tiles')
for color in range(6):
    for _y in range(8):
        for plane in range(4): a.emit(0xff if (color>>plane)&1 else 0)

a.label('nametable')
for y in range(24):
    for x in range(32):
        tile=0
        if 6<=x<=25 and 3<=y<=20:
            vx=x-6; vy=y-3; tile=1
            if vx in (0,19) or vy in (0,17): tile=2
            if 2<=vy<=15 and vx==9: tile=2
            if vy in (5,12) and vx==9: tile=3
            if vy in (6,11) and 3<=vx<=6: tile=2
            if vy in (6,11) and 13<=vx<=16: tile=2
            if (vx,vy)==(2,2): tile=4
            if (vx,vy)==(17,15): tile=5
        a.emit(tile,0)

a.label('blue_path')
for x,y in [(2,2),(3,2),(4,2),(5,2),(5,3),(4,3),(3,3),(2,3)]: a.emit(x,y)
a.label('red_path')
for x,y in [(17,15),(16,15),(15,15),(14,15),(14,16),(15,16),(16,16),(17,16)]: a.emit(x,y)

a.patch()
if MAIN + len(a.code) >= HEADER:
    raise SystemExit(f'code too large end={MAIN+len(a.code):04X}')
rom[MAIN:MAIN+len(a.code)] = a.code

rom[HEADER:HEADER+10] = b'TMR SEGA  '
rom[HEADER+0x0f] = (6<<4)|0x0c
chk=sum(rom[:HEADER]) & 0xffff
rom[HEADER+0x0a]=chk&0xff; rom[HEADER+0x0b]=(chk>>8)&0xff

out=Path(sys.argv[1] if len(sys.argv)>1 else 'build/gg_tick_probe.gg')
out.parent.mkdir(parents=True,exist_ok=True)
out.write_bytes(rom)
print(f'wrote {out} bytes={len(rom)} main_end=0x{MAIN+len(a.code):04X} checksum=0x{chk:04x}')
print('labels', {k:hex(v) for k,v in a.labels.items() if k in ('main','loop','tick','draw_xy','blue_path','red_path')})
