#!/usr/bin/env node

/**
 * IPSW Firmware Target Guide & Extraction Assistant
 * Part of Project DarwinFire for Redmi 12 ("fire")
 */

import https from 'https';

const RECOMMENDED_DEVICES = [
  {
    name: 'iPhone 7 (Global)',
    identifier: 'iPhone9,1',
    board: 'D10AP',
    arch: 'arm64 (Non-PAC)',
    chip: 'Apple A10 Fusion',
    compatibility: 'HIGHEST (No PAC instructions, last purely ARMv8 target)'
  },
  {
    name: 'iPhone 8 (Global)',
    identifier: 'iPhone10,1',
    board: 'D20AP',
    arch: 'arm64 (Non-PAC)',
    chip: 'Apple A11 Bionic',
    compatibility: 'HIGH (No PAC, supports iOS 16)'
  },
  {
    name: 'iPhone SE (1st gen)',
    identifier: 'iPhone8,4',
    board: 'N69uAP',
    arch: 'arm64 (Non-PAC)',
    chip: 'Apple A9',
    compatibility: 'GOOD (Lightweight RootFS, iOS 15)'
  }
];

function printBanner() {
  console.log(`\n============================================================`);
  console.log(`  DARWIN-FIRE IPSW FIRMWARE TARGET SELECTOR & EXTRACTOR`);
  console.log(`  Target Hardware: Xiaomi Redmi 12 ("fire" / MediaTek Helio G88)`);
  console.log(`============================================================\n`);
}

function showRecommendedTargets() {
  printBanner();
  console.log(`[+] RECOMMENDED FIRMWARE SOURCES FOR NON-PAC DEVICES:\n`);
  RECOMMENDED_DEVICES.forEach((dev, idx) => {
    console.log(`[${idx + 1}] ${dev.name} (${dev.identifier})`);
    console.log(`    Board Config:  ${dev.board}`);
    console.log(`    Architecture:  ${dev.arch}`);
    console.log(`    Apple Chip:    ${dev.chip}`);
    console.log(`    Compatibility: ${dev.compatibility}\n`);
  });

  console.log(`============================================================`);
  console.log(`  STEP-BY-STEP EXTRACTION PROCEDURE:`);
  console.log(`============================================================`);
  console.log(`
1. DOWNLOAD IPSW:
   Visit: https://ipsw.me/
   Select iPhone 7 or iPhone 8 -> Download the latest signed/unsigned IPSW (e.g. iOS 15.8.3 or iOS 16.7.10).

2. UNPACK ARCHIVE:
   Rename .ipsw to .zip or run:
     tar -xf iPhone_4.7_P3_15.8.3_Restore.ipsw

3. LOCATE ROOTFS DMG:
   Identify the largest .dmg file (typically 4GB - 6GB):
     dir *.dmg

4. MOUNT / EXTRACT ROOTFS:
   * On Linux/WSL:
     sudo apt install apfs-fuse 7zip
     7z x 078-XXXXX.dmg -o./rootfs_extracted/
   * Critical Binaries to Copy:
     ./rootfs_extracted/sbin/launchd
     ./rootfs_extracted/usr/lib/dyld
     ./rootfs_extracted/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64

5. VERIFY WITH DARWIN-FIRE ANALYZER:
   Run:
     npm run inspect ./rootfs_extracted/sbin/launchd
`);
}

showRecommendedTargets();
