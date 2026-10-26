/**
 * Generates synthetic Mach-O 64-bit binaries for testing tools/macho_analyzer.js
 */

import fs from 'fs';
import path from 'path';

function createMachOBinary(outputPath, { isPAC = false, use16K = true, addMetal = false }) {
  const buf = Buffer.alloc(4096);
  let offset = 0;

  // Header (32 bytes)
  buf.writeUInt32LE(0xfeedfacf, 0); // MH_MAGIC_64
  buf.writeUInt32LE(0x0100000c, 4); // CPU_TYPE_ARM64
  buf.writeUInt32LE(isPAC ? 2 : 0, 8); // CPU_SUBTYPE (2 = arm64e PAC, 0 = arm64_all)
  buf.writeUInt32LE(2, 12); // MH_EXECUTE
  
  const numCmds = addMetal ? 4 : 3;
  buf.writeUInt32LE(numCmds, 16); // ncmds
  buf.writeUInt32LE(300, 20); // sizeofcmds
  buf.writeUInt32LE(0x00200085, 24); // flags
  buf.writeUInt32LE(0, 28); // reserved
  offset = 32;

  // 1. LC_SEGMENT_64: __PAGEZERO (72 bytes)
  buf.writeUInt32LE(0x19, offset); // LC_SEGMENT_64
  buf.writeUInt32LE(72, offset + 4);
  buf.write('__PAGEZERO', offset + 8, 'ascii');
  buf.writeBigUInt64LE(0n, offset + 24); // vmaddr
  buf.writeBigUInt64LE(use16K ? 0x4000n : 0x1000n, offset + 32); // vmsize
  buf.writeBigUInt64LE(0n, offset + 40); // fileoff
  buf.writeBigUInt64LE(0n, offset + 48); // filesize
  buf.writeUInt32LE(0, offset + 56); // maxprot
  buf.writeUInt32LE(0, offset + 60); // initprot
  buf.writeUInt32LE(0, offset + 64); // nsects
  buf.writeUInt32LE(0, offset + 68); // flags
  offset += 72;

  // 2. LC_SEGMENT_64: __TEXT (72 bytes)
  buf.writeUInt32LE(0x19, offset);
  buf.writeUInt32LE(72, offset + 4);
  buf.write('__TEXT', offset + 8, 'ascii');
  buf.writeBigUInt64LE(use16K ? 0x4000n : 0x1000n, offset + 24);
  buf.writeBigUInt64LE(use16K ? 0x8000n : 0x2000n, offset + 32);
  buf.writeBigUInt64LE(0n, offset + 40);
  buf.writeBigUInt64LE(4096n, offset + 48);
  buf.writeUInt32LE(7, offset + 56); // rwx
  buf.writeUInt32LE(5, offset + 60); // rx
  buf.writeUInt32LE(0, offset + 64);
  buf.writeUInt32LE(0, offset + 68);
  offset += 72;

  // 3. LC_LOAD_DYLINKER (32 bytes)
  const dylinkerStr = '/usr/lib/dyld';
  buf.writeUInt32LE(0x0e, offset);
  buf.writeUInt32LE(32, offset + 4);
  buf.writeUInt32LE(12, offset + 8); // string offset
  buf.write(dylinkerStr, offset + 12, 'ascii');
  offset += 32;

  // 4. (Optional) LC_LOAD_DYLIB: Metal.framework
  if (addMetal) {
    const dylibStr = '/System/Library/Frameworks/Metal.framework/Metal';
    const cmdLen = 64;
    buf.writeUInt32LE(0x0c, offset);
    buf.writeUInt32LE(cmdLen, offset + 4);
    buf.writeUInt32LE(24, offset + 8); // string offset
    buf.write(dylibStr, offset + 24, 'ascii');
    offset += cmdLen;
  }

  const dir = path.dirname(outputPath);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  fs.writeFileSync(outputPath, buf);
  console.log(`[+] Generated test fixture: ${outputPath}`);
}

createMachOBinary('test/fixtures/arm64_compatible_launchd.bin', { isPAC: false, use16K: false, addMetal: false });
createMachOBinary('test/fixtures/arm64e_pac_springboard.bin', { isPAC: true, use16K: true, addMetal: true });
