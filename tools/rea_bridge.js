#!/usr/bin/env node

/**
 * REA (Reverse Engineer Anything) Bridge & Workflow Manager
 * Integrates https://github.com/morluto/rea with DarwinFire
 */

import { execSync } from 'child_process';
import fs from 'fs';
import path from 'path';

function printBanner() {
  console.log(`\n============================================================`);
  console.log(`  DARWIN-FIRE REA (REVERSE ENGINEER ANYTHING) INTEGRATION`);
  console.log(`  Upstream Repository: https://github.com/morluto/rea`);
  console.log(`============================================================\n`);
}

function showReaInstructions() {
  printBanner();
  console.log(`[+] PURPOSE:`);
  console.log(`    REA connects AI coding assistants and terminals to local reverse-engineering`);
  console.log(`    engines (Ghidra, IDA, Hopper) to inspect and decompile iOS Mach-O binaries.`);

  console.log(`\n[+] QUICK CONFIGURATION:`);
  console.log(`    To setup REA MCP agents on your machine, run:`);
  console.log(`      npx.cmd rea-agents setup\n`);

  console.log(`[+] ANALYSIS TARGETS FOR IOS USERLAND:`);
  console.log(`    1. /sbin/launchd`);
  console.log(`       - Reverse engineer main() to find early hardware checks.`);
  console.log(`       - Trace sandbox initialization & Mach port registration.`);
  console.log(`    2. /usr/lib/dyld`);
  console.log(`       - Analyze dyld::loadPhase5() to inspect 16KB page enforcement.`);
  console.log(`       - Find patch points to bypass page size abort on Android 4KB kernel.`);
  console.log(`    3. /System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer`);
  console.log(`       - Extract IOConnectCallMethod IDs for display framebuffer control.`);
  console.log(`       - Write C shims mapping these IDs to Linux DRM /dev/dri/card0.`);

  console.log(`\n[+] WORKFLOW TEMPLATE:`);
  console.log(`    When an extracted binary is placed in ./extracted_binaries/, you can run:`);
  console.log(`      node tools/rea_bridge.js --target ./extracted_binaries/launchd\n`);
}

const args = process.argv.slice(2);
if (args.includes('--setup')) {
  console.log(`[*] Spawning REA setup via npm/npx...`);
  try {
    execSync('npx.cmd rea-agents setup', { stdio: 'inherit' });
  } catch (err) {
    console.error(`[!] Error running rea-agents:`, err.message);
  }
} else {
  showReaInstructions();
}
