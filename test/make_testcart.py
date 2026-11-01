#!/usr/bin/env python3
"""Builds testcart.j64: a tiny self-written Jaguar program that exercises the
68000, the video interrupt, the blitter, the GPU, the DSP and the object processor.

Expected picture: black border, a blue 320x200 panel with a red 100x80 rectangle
(blitter) and a green 20x20 square (GPU). The DSP outputs a square wave."""
import struct, sys

ROM_BASE = 0x800000
CODE = 0x802000

# ---------------------------------------------------------------- 68000 ----
class M68k:
    def __init__(self, org):
        self.org, self.buf, self.labels, self.fix = org, bytearray(), {}, []
    def pc(self): return self.org + len(self.buf)
    def label(self, n): self.labels[n] = self.pc()
    def w(self, *ws):
        for v in ws: self.buf += struct.pack('>H', v & 0xFFFF)
    def l(self, v): self.buf += struct.pack('>I', v & 0xFFFFFFFF)
    def l_ref(self, name): self.fix.append(('abs32', len(self.buf), name)); self.l(0)
    def br(self, opc, name):          # Bcc/BRA/DBcc with 16-bit displacement
        self.w(opc); self.fix.append(('rel16', len(self.buf), name)); self.w(0)
    # instructions used by the test
    def move_l_imm_abs(self, imm, addr): self.w(0x23FC); self.l(imm); self.l(addr)
    def move_l_lbl_abs(self, name, addr): self.w(0x23FC); self.l_ref(name); self.l(addr)
    def move_w_imm_abs(self, imm, addr): self.w(0x33FC, imm); self.l(addr)
    def move_w_abs_d(self, addr, d): self.w(0x3039 | (d << 9)); self.l(addr)
    def lea_abs(self, addr, a): self.w(0x41F9 | (a << 9)); self.l(addr)
    def lea_lbl(self, name, a): self.w(0x41F9 | (a << 9)); self.l_ref(name)
    def moveq(self, n, d): self.w(0x7000 | (d << 9) | (n & 0xFF))
    def move_w_imm_d(self, imm, d): self.w(0x303C | (d << 9), imm)
    def copy_l(self): self.w(0x22D8)                     # move.l (a0)+,(a1)+
    def dbra(self, d, name): self.br(0x51C8 | d, name)
    def cmp_w(self, src, dst): self.w(0xB040 | (dst << 9) | src)
    def beq(self, n): self.br(0x6700, n)
    def bra(self, n): self.br(0x6000, n)
    def move_sr(self, v): self.w(0x46FC, v)
    def addq_l_abs(self, q, addr): self.w(0x50B9 | ((q & 7) << 9)); self.l(addr)
    def rte(self): self.w(0x4E73)
    def align(self, n):
        while len(self.buf) % n: self.buf.append(0)
    def blob(self, name, data): self.align(4); self.label(name); self.buf += data
    def resolve(self):
        for kind, off, name in self.fix:
            t = self.labels[name]
            if kind == 'abs32': struct.pack_into('>I', self.buf, off, t)
            else:
                d = t - (self.org + off)
                assert -32768 <= d < 32768
                struct.pack_into('>h', self.buf, off, d)
        return bytes(self.buf)

# ------------------------------------------------------------ GPU / DSP ----
class Risc:
    OPS = dict(add=0, addq=2, addqt=3, sub=4, subq=6, neg=8, moveq=35, move=34,
               movei=38, store=47, storew=46, jr=53, nop=57)
    def __init__(self, org): self.org, self.buf, self.labels, self.fix = org, bytearray(), {}, []
    def pc(self): return self.org + len(self.buf)
    def label(self, n): self.labels[n] = self.pc()
    def op(self, name, r1, r2): self.buf += struct.pack('>H', (self.OPS[name] << 10) | ((r1 & 31) << 5) | (r2 & 31))
    def movei(self, imm, r):
        self.op('movei', 0, r); self.buf += struct.pack('>HH', imm & 0xFFFF, (imm >> 16) & 0xFFFF)
    def jr(self, cc, name):
        self.fix.append((len(self.buf), name)); self.op('jr', 0, cc)
    def resolve(self):
        for off, name in self.fix:
            d = (self.labels[name] - (self.org + off + 2)) // 2
            assert -16 <= d <= 15, (name, d)
            v = struct.unpack_from('>H', self.buf, off)[0] | ((d & 31) << 5)
            struct.pack_into('>H', self.buf, off, v)
        while len(self.buf) % 4: self.buf += b'\0'
        return bytes(self.buf)

T, NE = 0x00, 0x01
FB = 0x100000
GREEN, BLUE, RED = 0x003F, 0x07C0, 0xF800   # Jaguar RGB16: RRRRR BBBBB GGGGGG

gpu = Risc(0xF03000)
gpu.movei(FB + (60 * 320 + 200) * 2, 1)
gpu.movei(GREEN, 2)
gpu.op('moveq', 20, 3)
gpu.label('row')
gpu.op('moveq', 20, 4)
gpu.op('move', 1, 5)
gpu.label('col')
gpu.op('storew', 5, 2)          # storew r2,(r5)
gpu.op('addqt', 2, 5)
gpu.op('subq', 1, 4)
gpu.jr(NE, 'col')
gpu.op('nop', 0, 0)
gpu.movei(640, 6)
gpu.op('add', 6, 1)
gpu.op('subq', 1, 3)
gpu.jr(NE, 'row')
gpu.op('nop', 0, 0)
gpu.movei(0x1FF00, 7)
gpu.movei(0xC0DE, 8)
gpu.op('store', 7, 8)           # store r8,(r7)
gpu.movei(0xF02114, 9)
gpu.op('moveq', 0, 10)
gpu.op('store', 9, 10)          # stop the GPU
gpu.op('nop', 0, 0)
gpu.label('halt')
gpu.jr(T, 'halt')
gpu.op('nop', 0, 0)
gpu_code = gpu.resolve()

dsp = Risc(0xF1B000)
dsp.movei(0xF1A148, 1)
dsp.movei(0xF1A14C, 2)
dsp.movei(0x2000, 3)
dsp.label('loop')
dsp.movei(300, 4)
dsp.label('delay')
dsp.op('subq', 1, 4)
dsp.jr(NE, 'delay')
dsp.op('nop', 0, 0)
dsp.op('neg', 0, 3)
dsp.op('store', 1, 3)
dsp.op('store', 2, 3)
dsp.jr(T, 'loop')
dsp.op('nop', 0, 0)
dsp_code = dsp.resolve()

# Object list: one 16bpp 320x200 bitmap + STOP
OL = 0x80000
ypos, height = 38 + 40, 200
p0 = 0 | (ypos << 3) | (height << 14) | (((OL + 16) >> 3) << 24) | ((FB >> 3) << 43)
p1 = 0 | (4 << 12) | (1 << 15) | (80 << 18) | (80 << 28)
objlist = struct.pack('>QQQ', p0, p1, 4)

m = M68k(CODE)
m.move_sr(0x2700)
m.move_l_lbl_abs('vbl', 0x100)
m.move_w_imm_abs(0x0687, 0xF00028)      # VMODE: RGB16, PWIDTH 4, BGEN, video on
m.move_w_imm_abs(0x0000, 0xF00058)      # BG black
m.move_w_imm_abs(500, 0xF0004E)         # VI in vertical blank
# Blitter: clear panel to blue, then red rectangle
for pix, step, patd, count in [(0, 0x0001FEC0, BLUE, (200 << 16) | 320),
                               ((20 << 16) | 20, 0x0001FF9C, RED, (80 << 16) | 100)]:
    m.move_l_imm_abs(FB, 0xF02200)          # A1_BASE
    m.move_l_imm_abs(0x00014220, 0xF02204)  # A1_FLAGS: 16bpp, width 320, XADD pixel
    m.move_l_imm_abs(pix, 0xF0220C)         # A1_PIXEL
    m.move_l_imm_abs(0, 0xF02218)           # A1_FPIXEL
    m.move_l_imm_abs(step, 0xF02210)        # A1_STEP
    m.move_l_imm_abs(patd << 16 | patd, 0xF02268)
    m.move_l_imm_abs(patd << 16 | patd, 0xF0226C)
    m.move_l_imm_abs(count, 0xF0223C)       # B_COUNT
    m.move_l_imm_abs(0x00010200, 0xF02238)  # B_CMD: PATDSEL | UPDA1
# Load and start the GPU and DSP
for lbl, ram, n, pcreg in [('gpucode', 0xF03000, len(gpu_code) // 4, 0xF02110),
                           ('dspcode', 0xF1B000, len(dsp_code) // 4, 0xF1A110)]:
    m.lea_lbl(lbl, 0); m.lea_abs(ram, 1); m.moveq(n - 1, 0)
    m.label('cp_' + lbl); m.copy_l(); m.dbra(0, 'cp_' + lbl)
    m.move_l_imm_abs(ram, pcreg)
    m.move_l_imm_abs(1, pcreg + 4)
# Object list, OLP (word-swapped), enable video interrupt
m.lea_lbl('ol', 0); m.lea_abs(OL, 1); m.moveq(5, 0)
m.label('cp_ol0'); m.copy_l(); m.dbra(0, 'cp_ol0')
m.move_l_imm_abs(((OL & 0xFFFF) << 16) | (OL >> 16), 0xF00020)
m.move_w_imm_abs(0x0001, 0xF000E0)
m.move_sr(0x2000)
m.label('main')
m.move_w_abs_d(0x1FF12, 1)
m.label('wait')
m.move_w_abs_d(0x1FF12, 0)
m.cmp_w(1, 0)
m.beq('wait')
m.lea_lbl('ol', 0); m.lea_abs(OL, 1); m.moveq(5, 0)
m.label('cp_ol'); m.copy_l(); m.dbra(0, 'cp_ol')
m.bra('main')
m.label('vbl')
m.addq_l_abs(1, 0x1FF10)
m.move_w_imm_abs(0x0101, 0xF000E0)      # clear video int, keep it enabled
m.rte()
m.blob('gpucode', gpu_code)
m.blob('dspcode', dsp_code)
m.blob('ol', objlist)
code = m.resolve()

rom = bytearray(b'\xFF' * 0x10000)
struct.pack_into('>II', rom, 0x400, 0x04040404, CODE)
rom[0x2000:0x2000 + len(code)] = code
out = sys.argv[1] if len(sys.argv) > 1 else 'testcart.j64'
open(out, 'wb').write(rom)
print(f'wrote {out}: 68k {len(code)} bytes, GPU {len(gpu_code)}, DSP {len(dsp_code)}')
