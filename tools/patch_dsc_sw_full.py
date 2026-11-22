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
    # Called from SWContext::set_destination_surface (0x188445304, right after Surface->vtable[+0x80]()):
    #   x20 = SWContext*, x19 = CA::OGL::Surface*, w0 = result of Surface->lock_cpu(&rb, &baseAddr)
    #   x21..x24 are already callee-saved by 0x188445294's prologue, and [sp] has scratch space.
    #
    # 1. If w0 != 0 (CPU offscreen Surface: BGRA backdrop/blur, L008 alpha mask, L555/L565):
    #    branch straight back to 0x188445308 so Apple's original SWContext renders ALL offscreen
    #    surfaces, masks (OrbFlashlight/OrbCamera/CC icons), and blurs natively!
    # 2. If w0 == 0 (display IOSurface where lock_cpu returns false):
    #    query Surface->vtable[+0x98]() -> IOSurfaceRef, dynamically read IOSurfaceGetBaseAddress()
    #    and IOSurfaceGetBytesPerRow(), read viewport [x20,#0xb8..#0xc4] (works at 720x1600 AND
    #    1080x2400 with zero hardcoding!), call SWContext::set_destination(), and return 1.
    insns = [
        0x34000040,                     # +0x00: cbz  w0, +0x08 (IOSurface fallback)
        enc_b(pc0 + 0x04, 0x188445308), # +0x04: b    0x188445308 (original CPU offscreen surface path)
        0xf9400268,                     # +0x08: ldr  x8, [x19]           (Surface vtable)
        0xf9404d08,                     # +0x0c: ldr  x8, [x8, #0x98]     (Surface::iosurface)
        0xaa1303e0,                     # +0x10: mov  x0, x19
        0xd63f0100,                     # +0x14: blr  x8                  (x0 = IOSurfaceRef)
        0xb5000040,                     # +0x18: cbnz x0, +0x08
        enc_b(pc0 + 0x1c, 0x1884453c0), # +0x1c: b    0x1884453c0         (return 1 if NULL)
        0xaa0003f5,                     # +0x20: mov  x21, x0             (save IOSurfaceRef)
        enc_bl(pc0 + 0x24, 0x18dbcc5a4),# +0x24: bl   IOSurfaceGetBaseAddress
        0xaa0003f6,                     # +0x28: mov  x22, x0             (save baseAddr)
        0xaa1503e0,                     # +0x2c: mov  x0, x21
        enc_bl(pc0 + 0x30, 0x18dbcc5d4),# +0x30: bl   IOSurfaceGetBytesPerRow
        0xaa0003e2,                     # +0x34: mov  x2, x0              (rowbytes)
        0xaa0003e5,                     # +0x38: mov  x5, x0              (alpha rowbytes)
        0xaa1603e1,                     # +0x3c: mov  x1, x22             (baseAddr)
        0xaa1603e4,                     # +0x40: mov  x4, x22             (alpha baseAddr)
        enc_movz_w(3, 32),              # +0x44: mov  w3, #32             (bpp)
        0x29571e86,                     # +0x48: ldp  w6, w7, [x20, #0xb8] (viewport x, y)
        0x29582688,                     # +0x4c: ldp  w8, w9, [x20, #0xc0] (viewport w, h)
        0x290027e8,                     # +0x50: stp  w8, w9, [sp]
        0xaa1403e0,                     # +0x54: mov  x0, x20             (SWContext*)
        enc_bl(pc0 + 0x58, 0x1884457f8),# +0x58: bl   SWContext::set_destination
        enc_b(pc0 + 0x5c, 0x1884453c0), # +0x5c: b    0x1884453c0         (return 1, epilogue)
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
        (0x1882eb0d8, tramp, 'SWContext::set_destination_surface offscreen-preserving IOSurface fallback trampoline'),
        (0x1884053c8, struct.pack('<I', 0x52800000), 'restore vtable +0x20 to mov w0, #0'),
        (0x1884053e4, struct.pack('<I', 0x52800020), 'mov w0, #1 (SWContext::supports_iosurface vtable +0x40)'),
        (0x1884053ec, struct.pack('<I', 0x92800000), 'mov x0, #-1 (SWContext::supported_surface_flags vtable +0x48)'),
        (0x188445294, struct.pack('<I', 0xd10283ff), 'RESTORE SWContext::set_destination_surface prologue (sub sp, sp, #0xa0)'),
        (0x188445304, struct.pack('<I', enc_b(0x188445304, 0x1882eb0d8)), 'b trampoline (only when Surface->lock_cpu fails -> display IOSurface)'),
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
