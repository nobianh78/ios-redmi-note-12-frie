#!/usr/bin/env node

/**
 * Mach-O 64-bit Binary Analyzer & Hardware Compatibility Verifier
 * Part of Project DarwinFire for Redmi 12 ("fire" / MT6769H)
 */

import fs from 'fs';
import path from 'path';

// Mach-O Constants
const MH_MAGIC = 0xfeedface;
const MH_CIGAM = 0xcefaedfe;
const MH_MAGIC_64 = 0xfeedfacf;
const MH_CIGAM_64 = 0xcffaedfe;
const FAT_MAGIC = 0xcafebabe;
const FAT_CIGAM = 0xbebafeca;

const CPU_TYPE_ARM = 12;
const CPU_TYPE_ARM64 = 0x01000000 | 12; // 0x0100000c
const CPU_SUBTYPE_ARM64_ALL = 0;
const CPU_SUBTYPE_ARM64_V8 = 1;
const CPU_SUBTYPE_ARM64E = 2; // Pointer Authentication (PAC)

const LC_SEGMENT = 0x01;
const LC_SYMTAB = 0x02;
const LC_SEGMENT_64 = 0x19;
const LC_ROUTINES_64 = 0x1a;
const LC_UUID = 0x1b;
const LC_CODE_SIGNATURE = 0x1d;
const LC_LOAD_DYLINKER = 0x0e;
const LC_LOAD_DYLIB = 0x0c;
const LC_MAIN = 0x80000028;
const LC_BUILD_VERSION = 0x32;

const PLATFORMS = {
  1: 'macOS',
  2: 'iOS',
  3: 'tvOS',
  4: 'watchOS',
  5: 'bridgeOS',
  6: 'MacCatalyst',
  7: 'iOSSimulator'
};

function parseMachO(filePath) {
  if (!fs.existsSync(filePath)) {
    console.error(`[!] Error: File not found: ${filePath}`);
    process.exit(1);
  }

  const buf = fs.readFileSync(filePath);
  if (buf.length < 32) {
    console.error(`[!] Error: File is too small to be a valid Mach-O binary.`);
    process.exit(1);
  }

  let offset = 0;
  const magicBE = buf.readUInt32BE(0);
  const magicLE = buf.readUInt32LE(0);
  
  let isLittleEndian = true;
  let is64 = false;

  console.log(`\n============================================================`);
  console.log(`  DARWIN-FIRE MACH-O ANALYZER & HARDWARE COMPATIBILITY CHECK`);
  console.log(`  Target Hardware: Xiaomi Redmi 12 ("fire" / MediaTek Helio G88)`);
  console.log(`============================================================`);
  console.log(`Analyzing file: ${path.resolve(filePath)}`);
  console.log(`File Size: ${(buf.length / 1024).toFixed(2)} KB (${buf.length} bytes)`);

  let sliceOffset = 0;
  let sliceBuf = buf;

  if (magicBE === FAT_MAGIC || magicBE === FAT_CIGAM || magicLE === FAT_MAGIC || magicLE === FAT_CIGAM) {
    const isFatBE = (magicBE === FAT_MAGIC || magicBE === FAT_CIGAM);
    const nfat_arch = isFatBE ? buf.readUInt32BE(4) : buf.readUInt32LE(4);
    console.log(`[+] Format: Universal / Fat Binary (${nfat_arch} architectures)`);
    
    let archOffset = 8;
    for (let a = 0; a < nfat_arch; a++) {
      const archCpu = isFatBE ? buf.readUInt32BE(archOffset) : buf.readUInt32LE(archOffset);
      const archSub = isFatBE ? buf.readUInt32BE(archOffset + 4) : buf.readUInt32LE(archOffset + 4);
      const archFileOff = isFatBE ? buf.readUInt32BE(archOffset + 8) : buf.readUInt32LE(archOffset + 8);
      const archSize = isFatBE ? buf.readUInt32BE(archOffset + 12) : buf.readUInt32LE(archOffset + 12);
      
      console.log(`    -> Slice #${a + 1}: CPU 0x${archCpu.toString(16)} Subtype 0x${archSub.toString(16)} at offset 0x${archFileOff.toString(16)} (${(archSize / 1024 / 1024).toFixed(2)} MB)`);
      if (archCpu === CPU_TYPE_ARM64) {
        sliceOffset = archFileOff;
        sliceBuf = buf.slice(archFileOff, archFileOff + archSize);
        console.log(`       [Selected ARM64 Slice for Deep Analysis]`);
        break;
      }
      archOffset += 20; // sizeof(struct fat_arch)
    }
  }

  const sMagicLE = sliceBuf.readUInt32LE(0);
  const sMagicBE = sliceBuf.readUInt32BE(0);

  if (sMagicLE === MH_MAGIC_64) {
    isLittleEndian = true;
    is64 = true;
    console.log(`[+] Architecture Slice: Mach-O 64-bit (Little Endian - ARM64 standard)`);
  } else if (sMagicBE === MH_MAGIC_64) {
    isLittleEndian = false;
    is64 = true;
    console.log(`[+] Architecture Slice: Mach-O 64-bit (Big Endian)`);
  } else if (sMagicLE === MH_MAGIC) {
    isLittleEndian = true;
    is64 = false;
    console.log(`[+] Architecture Slice: Mach-O 32-bit (Little Endian)`);
  } else if (sMagicBE === MH_MAGIC) {
    isLittleEndian = false;
    is64 = false;
    console.log(`[+] Architecture Slice: Mach-O 32-bit (Big Endian)`);
  } else {
    console.error(`[!] Unknown Magic in slice: 0x${sMagicBE.toString(16)}.`);
    return;
  }

  const readU32 = isLittleEndian ? (o) => sliceBuf.readUInt32LE(o) : (o) => sliceBuf.readUInt32BE(o);
  const readU64 = isLittleEndian ? (o) => sliceBuf.readBigUInt64LE(o) : (o) => sliceBuf.readBigUInt64BE(o);

  offset = 4;
  const cputype = readU32(offset); offset += 4;
  const cpusubtype = readU32(offset); offset += 4;
  const filetype = readU32(offset); offset += 4;
  const ncmds = readU32(offset); offset += 4;
  const sizeofcmds = readU32(offset); offset += 4;
  const flags = readU32(offset); offset += 4;
  if (is64) {
    offset += 4; // reserved
  }

  let cpuStr = 'Unknown';
  if (cputype === CPU_TYPE_ARM64) cpuStr = 'ARM64 (AArch64)';
  else if (cputype === CPU_TYPE_ARM) cpuStr = 'ARM (32-bit)';

  let subStr = 'Generic';
  const pacEnabled = (cpusubtype & 0x00ffffff) === CPU_SUBTYPE_ARM64E;
  if (pacEnabled) subStr = 'arm64e (Pointer Authentication - PAC Enabled)';
  else if ((cpusubtype & 0x00ffffff) === CPU_SUBTYPE_ARM64_V8) subStr = 'arm64_v8';
  else if ((cpusubtype & 0x00ffffff) === CPU_SUBTYPE_ARM64_ALL) subStr = 'arm64_all';

  console.log(`[+] CPU Architecture: ${cpuStr} | Subtype: ${subStr}`);
  console.log(`[+] Load Commands: ${ncmds} commands (${sizeofcmds} bytes total)`);

  const segments = [];
  const dylibs = [];
  let dylinker = 'None';
  let buildPlatform = 'Unknown';
  let minOS = 'Unknown';
  let has16KPageAlignment = false;

  let cmdOffset = offset;
  for (let i = 0; i < ncmds; i++) {
    if (cmdOffset + 8 > sliceBuf.length) break;
    const cmd = readU32(cmdOffset);
    const cmdsize = readU32(cmdOffset + 4);

    if (cmd === LC_SEGMENT_64) {
      const segname = sliceBuf.toString('ascii', cmdOffset + 8, cmdOffset + 24).replace(/\0/g, '');
      const vmaddr = readU64(cmdOffset + 24);
      const vmsize = readU64(cmdOffset + 32);
      const fileoff = readU64(cmdOffset + 40);
      const filesize = readU64(cmdOffset + 48);

      // Check alignment (16KB is 0x4000, 4KB is 0x1000)
      if (vmsize > 0n && (vmsize % 0x4000n === 0n)) {
        has16KPageAlignment = true;
      }

      segments.push({
        name: segname,
        vmaddr: `0x${vmaddr.toString(16)}`,
        vmsize: `0x${vmsize.toString(16)}`,
        filesize: `${filesize} bytes`
      });
    } else if (cmd === LC_LOAD_DYLIB) {
      const strOffset = readU32(cmdOffset + 8);
      const dylibName = sliceBuf.toString('ascii', cmdOffset + strOffset, cmdOffset + cmdsize).replace(/\0/g, '');
      dylibs.push(dylibName);
    } else if (cmd === LC_LOAD_DYLINKER) {
      const strOffset = readU32(cmdOffset + 8);
      dylinker = sliceBuf.toString('ascii', cmdOffset + strOffset, cmdOffset + cmdsize).replace(/\0/g, '');
    } else if (cmd === LC_BUILD_VERSION) {
      const platformId = readU32(cmdOffset + 8);
      const minosVal = readU32(cmdOffset + 12);
      buildPlatform = PLATFORMS[platformId] || `Platform ${platformId}`;
      const major = (minosVal >> 16) & 0xff;
      const minor = (minosVal >> 8) & 0xff;
      const patch = minosVal & 0xff;
      minOS = `${major}.${minor}.${patch}`;
    }

    cmdOffset += cmdsize;
  }

  console.log(`\n--- SEGMENTS (${segments.length}) ---`);
  segments.forEach(s => {
    console.log(`  * ${s.name.padEnd(16)} VMAddr: ${s.vmaddr.padEnd(14)} VMSize: ${s.vmsize.padEnd(12)} FileSize: ${s.filesize}`);
  });

  if (dylibs.length > 0) {
    console.log(`\n--- DEPENDENT DYLIBS (${dylibs.length}) ---`);
    dylibs.forEach(d => console.log(`  - ${d}`));
  }

  if (dylinker !== 'None') {
    console.log(`\n[+] Dynamic Linker: ${dylinker}`);
  }
  if (buildPlatform !== 'Unknown') {
    console.log(`[+] Build Target: ${buildPlatform} (Min OS Version: ${minOS})`);
  }

  // COMPATIBILITY EVALUATION FOR REDMI 12 (FIRE)
  console.log(`\n============================================================`);
  console.log(`  REDMI 12 ("fire" / Helio G88) COMPATIBILITY VERDICT:`);
  console.log(`============================================================`);

  let fatalCount = 0;
  let warnCount = 0;

  // 1. Check PAC
  if (pacEnabled) {
    console.log(`  [CRITICAL FAIL] PAC (arm64e) Detected!`);
    console.log(`    -> MediaTek Helio G88 (ARMv8.2-A) does not have PAC hardware.`);
    console.log(`    -> Result on device: Immediate SIGILL (Illegal Instruction crash).`);
    console.log(`    -> Fix: Target firmware from iPhone 6s/7/8 (arm64 standard) or strip PAC.`);
    fatalCount++;
  } else if (cputype === CPU_TYPE_ARM64) {
    console.log(`  [PASS] CPU Architecture: ARM64 standard (No PAC requirement). Compatible with Cortex-A75.`);
  } else {
    console.log(`  [FAIL] Architecture is not ARM64.`);
    fatalCount++;
  }

  // 2. Check Page Size
  if (has16KPageAlignment) {
    console.log(`  [WARNING] 16KB Page Alignment Detected!`);
    console.log(`    -> Android / Redmi 12 kernel runs default 4KB page size.`);
    console.log(`    -> Result on device: dyld abort or mmap alignment failure.`);
    console.log(`    -> Fix: Requires custom 16KB page emulation shim or modified dyld.`);
    warnCount++;
  }

  // 3. Check Dylibs
  const hasMetal = dylibs.some(d => d.includes('Metal'));
  if (hasMetal) {
    console.log(`  [CRITICAL FAIL] Metal Framework Dependency!`);
    console.log(`    -> Redmi 12 uses ARM Mali-G52 GPU with Vulkan/OpenGL.`);
    console.log(`    -> Result: UI cannot render without Metal-to-Vulkan translation layer.`);
    fatalCount++;
  }

  console.log(`\nSummary: ${fatalCount} Fatal Errors, ${warnCount} Warnings.`);
  if (fatalCount === 0) {
    console.log(`STATUS: Binary passes baseline structural compatibility check!\n`);
  } else {
    console.log(`STATUS: INCOMPATIBLE WITH REDMI 12 WITHOUT TRANSFORMATION SHIMS.\n`);
  }
}

const targetFile = process.argv[2];
if (!targetFile) {
  console.log(`Usage: node tools/macho_analyzer.js <path-to-macho-binary>`);
  console.log(`Example: node tools/macho_analyzer.js test/fixtures/sample_arm64_launchd.bin`);
  process.exit(0);
}

parseMachO(targetFile);
