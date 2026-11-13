#!/usr/bin/env node
/**
 * Turn a small dynamically-linked arm64 Mach-O (LC_MAIN + LC_LOAD_DYLINKER, as rust-lld/ld64.lld emit,
 * since lld has no -static yet) into a static one XNU can run as PID 1 without dyld:
 *   header (MH_EXECUTE, flags MH_NOUNDEFS) + every LC_SEGMENT_64 except __LINKEDIT + LC_UNIXTHREAD(pc=entry).
 * Segment bytes keep their file offsets and VAs, so PC-relative code and data stay valid; only the
 * header area in front of the first section is rewritten. Needs code that uses no imports (raw syscalls).
 * Usage: node tools/macho_static.js <in> <out>
 */
import fs from 'fs';

const LC_SEGMENT_64 = 0x19, LC_MAIN = 0x80000028, LC_UNIXTHREAD = 0x5;
const ARM_THREAD_STATE64 = 6, ARM_THREAD_STATE64_COUNT = 68;   // 29 x + fp lr sp pc (u64) + cpsr + pad (u32)

export function makeStatic(src) {
  if (src.readUInt32LE(0) !== 0xfeedfacf) throw new Error('not a 64-bit Mach-O');
  if (src.readUInt32LE(4) !== 0x0100000c) throw new Error('not arm64');
  if (src.readUInt32LE(12) !== 2) throw new Error('not MH_EXECUTE');
  const segs = [];
  let entryoff = null;
  for (let i = 0, p = 32, n = src.readUInt32LE(16); i < n; i++) {
    const cmd = src.readUInt32LE(p), size = src.readUInt32LE(p + 4);
    if (cmd === LC_SEGMENT_64) {
      const name = src.toString('ascii', p + 8, p + 24).replace(/\0.*/, '');
      segs.push({ name, raw: src.subarray(p, p + size), vmaddr: src.readBigUInt64LE(p + 24),
        fileoff: Number(src.readBigUInt64LE(p + 40)), filesize: Number(src.readBigUInt64LE(p + 48)),
        nsects: src.readUInt32LE(p + 64), at: p });
    } else if (cmd === LC_MAIN) {
      entryoff = src.readBigUInt64LE(p + 8);
    } else if (cmd === LC_UNIXTHREAD) {
      throw new Error('already has LC_UNIXTHREAD');
    }
    p += size;
  }
  if (entryoff === null) throw new Error('no LC_MAIN');
  const text = segs.find(s => s.name === '__TEXT');
  if (!text || text.fileoff !== 0) throw new Error('__TEXT must map file offset 0 (it holds the header)');
  const keep = segs.filter(s => s.name !== '__LINKEDIT');

  // First section byte in the file: the rewritten header must end before it.
  let firstSect = Infinity;
  for (const s of keep) {
    for (let k = 0; k < s.nsects; k++) {
      const sp = s.at + 72 + k * 80, off = src.readUInt32LE(sp + 48), size = Number(src.readBigUInt64LE(sp + 40));
      if (size && off) firstSect = Math.min(firstSect, off);
    }
  }

  const thread = Buffer.alloc(16 + ARM_THREAD_STATE64_COUNT * 4);
  thread.writeUInt32LE(LC_UNIXTHREAD, 0);
  thread.writeUInt32LE(thread.length, 4);
  thread.writeUInt32LE(ARM_THREAD_STATE64, 8);
  thread.writeUInt32LE(ARM_THREAD_STATE64_COUNT, 12);
  const pc = text.vmaddr + entryoff;
  thread.writeBigUInt64LE(pc, 16 + 32 * 8);                // x0..x28, fp, lr, sp, then pc

  const cmds = Buffer.concat([...keep.map(s => s.raw), thread]);
  if (32 + cmds.length > firstSect) throw new Error(`no room for the new header (${32 + cmds.length} > ${firstSect})`);

  const end = keep.reduce((m, s) => Math.max(m, s.fileoff + s.filesize), 0);
  const out = Buffer.alloc(end);
  src.copy(out, 0, 0, Math.min(end, src.length));
  out.fill(0, 0, firstSect);                              // drop the old header and load commands
  out.writeUInt32LE(0xfeedfacf, 0);
  out.writeUInt32LE(src.readUInt32LE(4), 4);
  out.writeUInt32LE(src.readUInt32LE(8), 8);
  out.writeUInt32LE(2, 12);                               // MH_EXECUTE
  out.writeUInt32LE(keep.length + 1, 16);
  out.writeUInt32LE(cmds.length, 20);
  out.writeUInt32LE(0x1, 24);                             // MH_NOUNDEFS; no MH_DYLDLINK / MH_PIE
  cmds.copy(out, 32);
  return { out, pc, segments: keep.map(s => s.name) };
}

if (process.argv[1].endsWith('macho_static.js')) {
  const [inp, outp] = process.argv.slice(2);
  if (!inp || !outp) { console.error('usage: node tools/macho_static.js <in> <out>'); process.exit(1); }
  const { out, pc, segments } = makeStatic(fs.readFileSync(inp));
  fs.writeFileSync(outp, out);
  console.log({ bytes: out.length, pc: '0x' + pc.toString(16), segments });
}
