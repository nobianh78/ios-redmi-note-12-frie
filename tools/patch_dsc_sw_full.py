#!/usr/bin/env python3
"""
DarwinFire: Redirect CA::WindowServer::IOMFBDisplay::get_context() to Apple's
built-in CPU Software Compositor (CA::OGL::SWContext) AND bridge
SWContext::set_destination_iosurface -> SWContext::set_destination(baseAddr, 4320, 32, 1080, 2400).

Patches inside dyld_shared_cache_arm64.03 (base block 637333 in ios16_root.img):
  1. 0x1882eafa4: cbz x0, #0x1882eb0cc  ->  nop
  2. 0x1882eafc4: bl #0x188487e28 (MetalContext)  ->  bl #0x1884409d0 (SWContext)
  3. 0x1882eb0d8..0x1882eb138 (dead EAGL fallback cave): trampoline bridging
     set_destination_iosurface(ctx, surface) -> IOSurfaceGetBaseAddress + SWContext::set_destination
  4. 0x1884053c8 (SWContext::set_destination_iosurface): b #0x1882eb0d8

Generates a 2-block (8 KB) Android sparse delta image `out/quartzcore_sw_full.simg`.
"""
import os
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

IMG_PATH = 'ios16_root.img'
OUT_SIMG = 'out/quartzcore_sw_full.simg'
BS = 4096

# dyld_shared_cache_arm64.03 starts at block 637333 in ios16_root.img.
# QuartzCore VA 0x188284000 is at foff 0x14f8000 in subcache .03:
# verify: 0x18850793c - 0x177b93c = 0x186d8c000 (VA - foff = 0x186d8c000)
SUB03_BASE_IMG = 637333 * BS
VA_TO_FOFF_03 = 0x186d8c000

def va_to_img(va):
    return SUB03_BASE_IMG + (va - VA_TO_FOFF_03)

def enc_b(pc, dst):
    imm = (dst - pc) >> 2
    assert -(1 << 25) <= imm < (1 << 25)
    return 0x14000000 | (imm & 0x3ffffff)

def enc_bl(pc, dst):
    imm = (dst - pc) >> 2
    assert -(1 << 25) <= imm < (1 << 25)
    return 0x94000000 | (imm & 0x3ffffff)

def enc_movz_w(rd, imm16):
    return 0x52800000 | ((imm16 & 0xffff) << 5) | (rd & 31)

def build_trampoline(pc0):
    # Called from SWContext::vtable[+0x358] (0x188445294):
    #   x0 = SWContext*, x1 = CA::OGL::Surface*
    # Extracts IOSurfaceRef via Surface->vtable[+0x98](), then IOSurfaceGetBaseAddress(),
    # and calls SWContext::set_destination(baseAddr, 4320, 32, 1080, 2400).
    insns = [
        0xd100c3ff,                     # +0x00: sub  sp, sp, #0x30
        0xa90153f3,                     # +0x04: stp  x19, x20, [sp, #0x10]
        0xa9027bfd,                     # +0x08: stp  x29, x30, [sp, #0x20]
        0xaa0003f3,                     # +0x0c: mov  x19, x0   (SWContext*)
        0xaa0103f4,                     # +0x10: mov  x20, x1   (Surface*)
        0xd2800000,                     # +0x14: mov  x0, #0
        0xb40000f4,                     # +0x18: cbz  x20, +0x1c (-> +0x34)
        0xf9400288,                     # +0x1c: ldr  x8, [x20]
        0xf9404d08,                     # +0x20: ldr  x8, [x8, #0x98] (Surface::iosurface)
        0xaa1403e0,                     # +0x24: mov  x0, x20
        0xd63f0100,                     # +0x28: blr  x8
        0xb4000040,                     # +0x2c: cbz  x0, +0x08 (-> +0x34)
        enc_bl(pc0 + 0x30, 0x18dbcc5a4),# +0x30: bl   IOSurfaceGetBaseAddress
        0xaa0003e1,                     # +0x34: mov  x1, x0    (baseAddr)
        0xaa0003e4,                     # +0x38: mov  x4, x0    (baseAddr)
        0xaa1303e0,                     # +0x3c: mov  x0, x19   (SWContext*)
        enc_movz_w(2, 4320),            # +0x40: mov  w2, #4320 (rowbytes)
        enc_movz_w(5, 4320),            # +0x44: mov  w5, #4320 (rowbytes)
        enc_movz_w(3, 32),              # +0x48: mov  w3, #32   (bpp)
        enc_movz_w(6, 0),               # +0x4c: mov  w6, #0
        enc_movz_w(7, 0),               # +0x50: mov  w7, #0
        enc_movz_w(10, 1080),           # +0x54: mov  w10, #1080 (width)
        enc_movz_w(11, 2400),           # +0x58: mov  w11, #2400 (height)
        0x29002fea,                     # +0x5c: stp  w10, w11, [sp]
        enc_bl(pc0 + 0x60, 0x1884457f8),# +0x60: bl   SWContext::set_destination
        enc_movz_w(0, 1),               # +0x64: mov  w0, #1    (return true)
        0xa94153f3,                     # +0x68: ldp  x19, x20, [sp, #0x10]
        0xa9427bfd,                     # +0x6c: ldp  x29, x30, [sp, #0x20]
        0x9100c3ff,                     # +0x70: add  sp, sp, #0x30
        0xd65f03c0,                     # +0x74: ret
    ]
    return b''.join(struct.pack('<I', w) for w in insns)

def main():
    tramp = build_trampoline(0x1882eb0d8)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    print('=== Disassembly verification of trampoline @ 0x1882eb0d8 ===')
    for ins in md.disasm(tramp, 0x1882eb0d8):
        print(f'  {hex(ins.address)}: {ins.mnemonic:8s} {ins.op_str}')

    patches = [
        (0x1882eafa4, struct.pack('<I', 0xd503201f), 'nop (skip EAGL fallback)'),
        (0x1882eafc4, struct.pack('<I', enc_bl(0x1882eafc4, 0x1884409d0)), 'bl SWContext::new'),
        (0x1882eb0d8, tramp, 'SWContext::set_destination_surface -> Surface::iosurface -> SWContext::set_destination trampoline'),
        (0x1884053c8, struct.pack('<I', 0x52800000), 'restore vtable +0x20 to mov w0, #0'),
        (0x1884053e4, struct.pack('<I', 0x52800020), 'mov w0, #1 (SWContext::supports_iosurface vtable +0x40)'),
        (0x1884053ec, struct.pack('<I', 0x92800000), 'mov x0, #-1 (SWContext::supported_surface_flags vtable +0x48)'),
        (0x188445294, struct.pack('<I', enc_b(0x188445294, 0x1882eb0d8)), 'b trampoline (SWContext::set_destination_surface vtable +0x358)'),
    ]

    total_blocks = os.path.getsize(IMG_PATH) // BS
    touched_blocks = set()
    with open(IMG_PATH, 'r+b') as f:
        for va, data, desc in patches:
            off = va_to_img(va)
            f.seek(off)
            old = f.read(len(data))
            print(f'[+] Writing {len(data)}B at VA {hex(va)} (img {hex(off)}): {desc}')
            f.seek(off)
            f.write(data)
            for b in range(off // BS, (off + len(data) - 1) // BS + 1):
                touched_blocks.add(b)

        blocks = sorted(touched_blocks)
        print(f'[*] Touched blocks: {blocks}')
        chunks = []
        pos = 0
        for b in blocks:
            if b > pos:
                chunks.append(('skip', pos, b))
            chunks.append(('raw', b, b + 1))
            pos = b + 1
        if pos < total_blocks:
            chunks.append(('skip', pos, total_blocks))

        os.makedirs(os.path.dirname(OUT_SIMG), exist_ok=True)
        with open(OUT_SIMG, 'wb') as out:
            out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BS, total_blocks, len(chunks), 0))
            for kind, s, e in chunks:
                n = e - s
                if kind == 'skip':
                    out.write(struct.pack('<HHII', 0xCAC3, 0, n, 12))
                else:
                    out.write(struct.pack('<HHII', 0xCAC1, 0, n, 12 + n * BS))
                    f.seek(s * BS)
                    out.write(f.read(n * BS))

    print(f'[+] Wrote delta sparse image: {OUT_SIMG} ({os.path.getsize(OUT_SIMG)} bytes, {len(blocks)} blocks)')

if __name__ == '__main__':
    main()
