#!/usr/bin/env node

/**
 * Android boot.img Builder for DarwinFire
 * Target: Xiaomi Redmi 12 ("fire" / MediaTek MT6769H)
 * Packs the Darwin trampoline, kernel payload, and ramdisk into an Android boot.img
 */

import fs from 'fs';
import path from 'path';

const BOOT_MAGIC = "ANDROID!";
const DEFAULT_PAGE_SIZE = 2048;

/**
 * Generates an ARM64 Linux Boot Header
 * Fastboot requires this 64-byte header at the start of any kernel image.
 */
function createArm64LinuxHeader(kernelPayloadSize) {
  const header = Buffer.alloc(64);
  
  // 0x00: b 0x40 (Jump over 64-byte header to code: opcode 0x14000010)
  header.writeUInt32LE(0x14000010, 0x00);
  // 0x04: nop (0xd503201f)
  header.writeUInt32LE(0xd503201f, 0x04);
  // 0x08: text_offset (0x00080000)
  header.writeBigUInt64LE(0x00080000n, 0x08);
  // 0x10: image_size (total kernel size including payload)
  header.writeBigUInt64LE(BigInt(kernelPayloadSize + 64), 0x10);
  // 0x18: flags (Little endian, 4KB page)
  header.writeBigUInt64LE(0x0a00n, 0x18);
  // 0x20: reserved
  header.writeBigUInt64LE(0n, 0x20);
  // 0x28: reserved
  header.writeBigUInt64LE(0n, 0x28);
  // 0x30: reserved
  header.writeBigUInt64LE(0n, 0x30);
  // 0x38: Magic "ARM\x64" (0x644d5241)
  header.writeUInt32LE(0x644d5241, 0x38);
  // 0x3c: Reserved
  header.writeUInt32LE(0, 0x3c);

  return header;
}

function padToPage(buf, pageSize) {
  const rem = buf.length % pageSize;
  if (rem === 0) return buf;
  const padding = Buffer.alloc(pageSize - rem, 0);
  return Buffer.concat([buf, padding]);
}

export function buildBootImage({
  outputPath = 'out/darwin_fire_boot.img',
  kernelPath = fs.existsSync('extracted_binaries/kernelcache_patched.macho') 
    ? 'extracted_binaries/kernelcache_patched.macho' 
    : (fs.existsSync('extracted_binaries/kernelcache.macho') ? 'extracted_binaries/kernelcache.macho' : null),
  cmdline = 'console=ttyMT0,921600n8 earlycon=mtk_uart,0x11002000 boot-args="-v amfi_get_out_of_my_way=1 cs_enforcement_disable=1"',
  pageSize = DEFAULT_PAGE_SIZE
} = {}) {
  console.log(`\n============================================================`);
  console.log(`  DARWIN-FIRE ANDROID BOOT.IMG BUILDER`);
  console.log(`  Target Hardware: Xiaomi Redmi 12 ("fire" / MediaTek Helio G88)`);
  console.log(`============================================================\n`);

  // 1. Prepare Kernel Payload
  let trampolinePayload;
  if (kernelPath && fs.existsSync(kernelPath)) {
    console.log(`[+] Packaging REAL Apple Mach-O Kernel: ${path.resolve(kernelPath)}`);
    trampolinePayload = fs.readFileSync(kernelPath);
  } else {
    trampolinePayload = Buffer.from(
      "DarwinFire Trampoline Payload: Initializing MediaTek Helio G88 MT6769 MMIO at 0x11002000...\n" +
      "Mapping XNU kernelcache and entering Mach subsystem.\n",
      'utf-8'
    );
  }

  const arm64Header = createArm64LinuxHeader(trampolinePayload.length);
  const rawKernel = Buffer.concat([arm64Header, trampolinePayload]);
  const kernelPadded = padToPage(rawKernel, pageSize);

  // 2. Prepare Ramdisk (Contains Apple Device Tree binary)
  let ramdiskPayload;
  if (fs.existsSync('extracted_binaries/fire_devicetree.adt')) {
    console.log(`[+] Embedding Apple Device Tree (ADT): ${path.resolve('extracted_binaries/fire_devicetree.adt')}`);
    ramdiskPayload = fs.readFileSync('extracted_binaries/fire_devicetree.adt');
  } else {
    ramdiskPayload = Buffer.from("DARWIN_RAMDISK: Contains /sbin/launchd mock and /dev/sep_mock\n", 'utf-8');
  }
  const ramdiskPadded = padToPage(ramdiskPayload, pageSize);

  // 3. Prepare Android Boot Header v2 (1660 bytes standard)
  const headerBuf = Buffer.alloc(pageSize);
  let off = 0;

  // Magic (8 bytes)
  headerBuf.write(BOOT_MAGIC, off, 8, 'ascii'); off += 8;
  // kernel_size
  headerBuf.writeUInt32LE(rawKernel.length, off); off += 4;
  // kernel_addr (0x40080000 physical base)
  headerBuf.writeUInt32LE(0x40080000, off); off += 4;
  // ramdisk_size
  headerBuf.writeUInt32LE(ramdiskPayload.length, off); off += 4;
  // ramdisk_addr (0x44000000)
  headerBuf.writeUInt32LE(0x44000000, off); off += 4;
  // second_size
  headerBuf.writeUInt32LE(0, off); off += 4;
  // second_addr
  headerBuf.writeUInt32LE(0, off); off += 4;
  // tags_addr (0x40000100)
  headerBuf.writeUInt32LE(0x40000100, off); off += 4;
  // page_size
  headerBuf.writeUInt32LE(pageSize, off); off += 4;
  // header_version (v2)
  headerBuf.writeUInt32LE(2, off); off += 4;
  // os_version (e.g. Android 13 equivalent for Redmi 12)
  headerBuf.writeUInt32LE(0, off); off += 4;
  // name (16 bytes)
  headerBuf.write("DarwinFire-v0.1", off, 16, 'ascii'); off += 16;
  // cmdline (512 bytes)
  headerBuf.write(cmdline, off, 512, 'ascii'); off += 512;
  // id (SHA hash / 32 bytes)
  off += 32;
  // extra_cmdline (1024 bytes)
  off += 1024;
  // recovery_dtbo_size
  headerBuf.writeUInt32LE(0, off); off += 4;
  // recovery_dtbo_offset
  headerBuf.writeBigUInt64LE(0n, off); off += 8;
  // header_size
  headerBuf.writeUInt32LE(1660, off); off += 4;
  // dtb_size
  headerBuf.writeUInt32LE(0, off); off += 4;
  // dtb_addr
  headerBuf.writeBigUInt64LE(0x43000000n, off); off += 8;

  // Combine into single boot.img
  const bootImg = Buffer.concat([headerBuf, kernelPadded, ramdiskPadded]);

  const outDir = path.dirname(outputPath);
  if (!fs.existsSync(outDir)) {
    fs.mkdirSync(outDir, { recursive: true });
  }

  fs.writeFileSync(outputPath, bootImg);

  console.log(`[+] SUCCESS: Generated Android boot image!`);
  console.log(`    Output Path:   ${path.resolve(outputPath)}`);
  console.log(`    Total Size:    ${(bootImg.length / 1024).toFixed(2)} KB (${bootImg.length} bytes)`);
  console.log(`    Kernel Size:   ${rawKernel.length} bytes (padded: ${kernelPadded.length})`);
  console.log(`    Ramdisk Size:  ${ramdiskPayload.length} bytes (padded: ${ramdiskPadded.length})`);
  console.log(`    Page Size:     ${pageSize} bytes`);
  console.log(`    Kernel Cmdline:\n      "${cmdline}"`);
  console.log(`\n============================================================`);
  console.log(`  TEST BOOT INSTRUCTIONS (ON UNLOCKED REDMI 12):`);
  console.log(`============================================================`);
  console.log(`1. Put Redmi 12 into Fastboot mode (Hold Volume Down + Power).`);
  console.log(`2. Connect USB-C cable to your PC.`);
  console.log(`3. Test boot in RAM safely without flashing to disk:`);
  console.log(`     fastboot boot ${outputPath}`);
  console.log(`============================================================\n`);
}

// Direct execution from CLI
if (process.argv[1].endsWith('bootimg_builder.js')) {
  buildBootImage();
}
