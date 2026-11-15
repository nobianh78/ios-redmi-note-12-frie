#!/usr/bin/env node
/**
 * Flatten a thin MH_EXECUTE XNU kernelcache into one contiguous memory image (laid out by VA),
 * and neutralise accesses to Apple implementation-defined system registers (op0=3, CRn=15:
 * HID/ACC/PMCR...), which do not exist on the MT6768's Cortex cores and would trap at EL1.
 *   MRS Xt, S3_x_C15_x_x  ->  MOVZ Xt, #0
 *   MSR S3_x_C15_x_x, Xt  ->  NOP
 * Usage: node tools/xnu_flatten.js <thin.macho> <out.flat>   (also writes <out.flat>.json)
 */
import fs from 'fs';

const PAGE = 0x4000n;
const NOP = 0xd503201f;

export function isImpDefSysreg(w) {
  // 1101010100 L 1 o0 op1 CRn CRm op2 Rt  with op0 = 2 + o0 = 3 and CRn = 15
  return ((w & 0xffd00000) >>> 0) === 0xd5100000 && ((w >>> 19) & 1) === 1 && ((w >>> 12) & 0xf) === 15;
}

export function patchWord(w) {
  if (!isImpDefSysreg(w)) return null;
  const isMrs = (w >>> 21) & 1;
  return isMrs ? ((0xd2800000 | (w & 0x1f)) >>> 0) : NOP;
}

export function flatten(macho) {
  if (macho.readUInt32LE(0) !== 0xfeedfacf) throw new Error('not a thin 64-bit Mach-O (strip FAT first)');
  if (macho.readUInt32LE(12) !== 2) throw new Error('not MH_EXECUTE (fileset kernelcaches are not supported yet)');

  const segs = [];
  let entry = null;
  let p = 32;
  for (let i = 0, n = macho.readUInt32LE(16); i < n; i++) {
    const cmd = macho.readUInt32LE(p), size = macho.readUInt32LE(p + 4);
    if (cmd === 0x19) {
      segs.push({
        name: macho.toString('ascii', p + 8, p + 24).replace(/\0.*/, ''),
        va: macho.readBigUInt64LE(p + 24), vsize: macho.readBigUInt64LE(p + 32),
        fileoff: Number(macho.readBigUInt64LE(p + 40)), filesize: Number(macho.readBigUInt64LE(p + 48)),
      });
    } else if (cmd === 0x5) {
      entry = macho.readBigUInt64LE(p + 16 + 256);   // LC_UNIXTHREAD: flavor,count, then x0..x28,fp,lr,sp,pc
    }
    p += size;
  }
  if (entry === null) throw new Error('no LC_UNIXTHREAD entry point');

  const live = segs.filter(s => s.vsize > 0n);
  const virtBase = live.reduce((m, s) => (s.va < m ? s.va : m), live[0].va) & ~(PAGE - 1n);
  const top = live.reduce((m, s) => (s.va + s.vsize > m ? s.va + s.vsize : m), 0n);
  const size = Number(((top - virtBase) + PAGE - 1n) & ~(PAGE - 1n));
  const flat = Buffer.alloc(size);

  const patches = {};
  for (const s of live) {
    const dst = Number(s.va - virtBase);
    macho.copy(flat, dst, s.fileoff, s.fileoff + Math.min(s.filesize, Number(s.vsize)));
    if (!s.name.endsWith('TEXT_EXEC')) continue;    // code only: __TEXT_EXEC (kernel) and __PLK_TEXT_EXEC (kexts)
    let count = 0;
    for (let o = dst; o + 4 <= dst + Number(s.vsize); o += 4) {
      const nw = patchWord(flat.readUInt32LE(o));
      if (nw !== null) { flat.writeUInt32LE(nw, o); count++; }
    }
    patches[s.name] = count;
  }

  return { flat, meta: { virtBase: '0x' + virtBase.toString(16), entry: '0x' + entry.toString(16),
                         entryOffset: Number(entry - virtBase), size, patches } };
}

/* Ramdisk payload read by kernel/heartbeat (struct xnu_payload in heartbeat.c):
 *   0x00 "XNUFLAT1"  0x08 virtBase  0x10 entryOffset  0x18 kernelSize  0x20 adtOffset  0x28 adtSize
 *   kernel image at +0x4000 (16K aligned, becomes physBase), ADT right after it (16K aligned). */
/* Optional ramdisk (the root device) goes between the kernel and the ADT, 16K aligned, so the loader copies
 * it with everything else. Its kernel VA (virtBase + flat.length) and size are written into the ADT's
 * /chosen/memory-map "RAMDisk" placeholder (adt_builder.py); IOKitBSDInit turns that into /dev/md0. */
export function patchRamdisk(adt, va, size) {
  const name = Buffer.alloc(32);
  name.write('RAMDisk', 'ascii');
  const at = adt.indexOf(name);
  if (at < 0 || adt.readUInt32LE(at + 32) !== 16) throw new Error('ADT has no 16-byte RAMDisk property');
  const out = Buffer.from(adt);
  out.writeBigUInt64LE(va, at + 36);
  out.writeBigUInt64LE(BigInt(size), at + 44);
  return out;
}

export function pack(flat, meta, adt, ramdisk = null) {
  const HDR = 0x4000;
  let rd = Buffer.alloc(0);
  if (ramdisk) {
    rd = Buffer.alloc(Math.ceil(ramdisk.length / 0x4000) * 0x4000);
    ramdisk.copy(rd);
    adt = patchRamdisk(adt, BigInt(meta.virtBase) + BigInt(flat.length), rd.length);
  }
  flat = Buffer.concat([flat, rd]);
  const adtOff = HDR + flat.length;
  const hdr = Buffer.alloc(HDR);
  hdr.write('XNUFLAT1', 0, 'ascii');
  hdr.writeBigUInt64LE(BigInt(meta.virtBase), 0x08);
  hdr.writeBigUInt64LE(BigInt(meta.entryOffset), 0x10);
  hdr.writeBigUInt64LE(BigInt(flat.length), 0x18);
  hdr.writeBigUInt64LE(BigInt(adtOff), 0x20);
  hdr.writeBigUInt64LE(BigInt(adt.length), 0x28);
  return Buffer.concat([hdr, flat, adt]);
}

if (process.argv[1].endsWith('xnu_flatten.js')) {
  const [inp, out, adtPath, rootPath] = process.argv.slice(2);
  if (!inp || !out) { console.error('usage: node tools/xnu_flatten.js <thin.macho> <out.flat> [adt [root-ramdisk]] (adt -> also writes <out.flat>.ramdisk)'); process.exit(1); }
  const { flat, meta } = flatten(fs.readFileSync(inp));
  fs.writeFileSync(out, flat);
  fs.writeFileSync(out + '.json', JSON.stringify(meta, null, 2));
  if (adtPath) fs.writeFileSync(out + '.ramdisk', pack(flat, meta, fs.readFileSync(adtPath), rootPath ? fs.readFileSync(rootPath) : null));
  console.log(meta);
}
