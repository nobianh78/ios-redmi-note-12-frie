#!/usr/bin/env node

/**
 * Android boot.img Builder for DarwinFire
 * Target: Xiaomi Redmi 12 ("fire" / MediaTek MT6769H)
 * Packs the Darwin trampoline, kernel payload, and ramdisk into an Android boot.img
 */

import fs from 'fs';
import path from 'path';
import zlib from 'zlib';

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
  // bootopt is parsed by MTK LK to pick the 64-bit kernel jump; without it LK never enters our AArch64 code.
  cmdline = 'bootopt=64S3,32N2,64N2 console=ttyMT0,921600n8 earlycon=mtk_uart,0x11002000',
  // Header v2 on fire requires a DTB in boot.img (from recovery_device_xiaomi_fire prebuilt/dtb.img).
  dtbPath = 'extracted_binaries/fire_dt/dtb.img',
  ramdiskPath = 'extracted_binaries/fire_devicetree.adt',
  appendPath = null,
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

  // Payloads that already carry the ARM64 header (magic "ARM\x64" at 0x38, e.g. kernel/heartbeat) are used as-is.
  const hasHeader = trampolinePayload.length >= 64 && trampolinePayload.readUInt32LE(0x38) === 0x644d5241;
  let image = hasHeader ? trampolinePayload
    : Buffer.concat([createArm64LinuxHeader(trampolinePayload.length), trampolinePayload]);
  if (appendPath) {
    // XNU payload rides in the gzip'd kernel: header+code padded to 16K, so it lands at the
    // 16K page after the code (load address is 16K aligned) where heartbeat.c looks for it.
    const payload = fs.readFileSync(appendPath);
    image = Buffer.concat([padToPage(image, 0x4000), payload]);
    image.writeBigUInt64LE(BigInt(image.length), 0x10);   // image_size covers the payload
    console.log(`[+] Appended payload: ${path.resolve(appendPath)} (${(payload.length / 1048576).toFixed(1)} MB)`);
  }
  // fire's LK only boots Image.gz (as TWRP/stock do); a raw Image made slot A fail and fall back.
  const rawKernel = zlib.gzipSync(image, { level: 9 });
  const kernelPadded = padToPage(rawKernel, pageSize);

  // 2. Prepare Ramdisk (Contains Apple Device Tree binary)
  let ramdiskPayload;
  if (fs.existsSync(ramdiskPath)) {
    console.log(`[+] Ramdisk: ${path.resolve(ramdiskPath)}`);
    ramdiskPayload = fs.readFileSync(ramdiskPath);
  } else {
    ramdiskPayload = Buffer.from("DARWIN_RAMDISK: Contains /sbin/launchd mock and /dev/sep_mock\n", 'utf-8');
  }
  const ramdiskPadded = padToPage(ramdiskPayload, pageSize);

  if (!fs.existsSync(dtbPath)) throw new Error(`Missing DTB ${dtbPath}: LK on fire refuses header v2 images without one`);
  const dtb = fs.readFileSync(dtbPath);
  const dtbPadded = padToPage(dtb, pageSize);

  // fire BoardConfig: base 0x40078000, kernel +0x8000, ramdisk +0x07c08000, tags +0x0bc08000, dtb +0x01f00000 (mkbootimg default)
  const BASE = 0x40078000;

  // 3. Prepare Android Boot Header v2 (1660 bytes standard)
  const headerBuf = Buffer.alloc(pageSize);
  let off = 0;

  // Magic (8 bytes)
  headerBuf.write(BOOT_MAGIC, off, 8, 'ascii'); off += 8;
  // kernel_size
  headerBuf.writeUInt32LE(rawKernel.length, off); off += 4;
  // kernel_addr
  headerBuf.writeUInt32LE(BASE + 0x8000, off); off += 4;
  // ramdisk_size
  headerBuf.writeUInt32LE(ramdiskPayload.length, off); off += 4;
  // ramdisk_addr
  headerBuf.writeUInt32LE(BASE + 0x07c08000, off); off += 4;
  // second_size
  headerBuf.writeUInt32LE(0, off); off += 4;
  // second_addr
  headerBuf.writeUInt32LE(0, off); off += 4;
  // tags_addr
  headerBuf.writeUInt32LE(BASE + 0x0bc08000, off); off += 4;
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
  headerBuf.writeUInt32LE(dtb.length, off); off += 4;
  // dtb_addr
  headerBuf.writeBigUInt64LE(BigInt(BASE + 0x01f00000), off); off += 8;

  // v2 layout: header | kernel | ramdisk | second(0) | recovery_dtbo(0) | dtb
  const bootImg = Buffer.concat([headerBuf, kernelPadded, ramdiskPadded, dtbPadded]);

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
  // Optional: node bootimg_builder.js <kernel.bin> <out.img> [ramdisk|-] [append-to-kernel]
  const [kernelPath, outputPath, rd, appendPath] = process.argv.slice(2);
  const ramdiskPath = rd && rd !== '-' ? rd : undefined;
  buildBootImage(kernelPath ? { kernelPath, outputPath, ramdiskPath, appendPath } : {});
}
