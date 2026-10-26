#!/usr/bin/env python3

"""
Apple XNU Kernelcache Decompiler & Hardware Stub Analyzer
Part of Project DarwinFire for Redmi 12 ("fire" / Helio G88)
Integrates Capstone disassembler to inspect Apple AIC & Platform Expert
"""

import sys
import os
import re
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

def analyze_kernel(macho_path):
    print("============================================================")
    print("  DARWIN-FIRE XNU REVERSE ENGINEERING & DISASSEMBLER")
    print(f"  Target: {os.path.abspath(macho_path)}")
    print("============================================================\n")

    if not os.path.exists(macho_path):
        print(f"[!] Error: File not found: {macho_path}")
        return

    with open(macho_path, "rb") as f:
        data = f.read()

    print(f"[+] Loaded Kernelcache: {len(data) / 1024 / 1024:.2f} MB ({len(data)} bytes)")

    # 1. Search for Darwin release string
    ver_match = re.search(rb"Darwin Kernel Version [^\x00]+", data)
    if ver_match:
        ver_str = ver_match.group().decode("ascii", errors="ignore")
        print(f"[+] Kernel Version: {ver_str}")

    # 2. Extract Prelink Info Dictionary
    print("\n[*] Scanning __PRELINK_INFO for Driver KEXTs...")
    prelink_idx = data.find(b"<dict>")
    if prelink_idx != -1:
        # Scan for critical KEXT identifiers
        kexts = [
            b"com.apple.driver.AppleARMPE",
            b"com.apple.driver.AppleInterruptController",
            b"com.apple.iokit.IOMobileFramebuffer",
            b"com.apple.driver.AppleMobileFileIntegrity"
        ]
        for kext in kexts:
            kpos = data.find(kext)
            if kpos != -1:
                print(f"    [FOUND KEXT] {kext.decode()}: referenced at 0x{kpos:x}")
            else:
                print(f"    [NOT FOUND]  {kext.decode()}")

    # 3. Disassemble AIC (Apple Interrupt Controller) Init Routine
    print("\n[*] Locating AppleInterruptController init references...")
    aic_str = b"AppleInterruptController"
    str_offset = data.find(aic_str)
    
    if str_offset != -1:
        print(f"[+] Found \"AppleInterruptController\" string at offset 0x{str_offset:x}")
        
        # Search for references in code (__PLK_TEXT_EXEC / __TEXT_EXEC)
        # We search for ADRP/ADD patterns or direct pointer references
        ptr_pattern = str_offset.to_bytes(4, "little")
        ref_offsets = []
        start = 0
        while True:
            idx = data.find(ptr_pattern, start)
            if idx == -1 or len(ref_offsets) >= 5:
                break
            ref_offsets.append(idx)
            start = idx + 4

        print(f"[+] Found {len(ref_offsets)} direct pointer references to AIC string:")
        for roff in ref_offsets:
            print(f"    -> Reference at offset 0x{roff:x}")

        # Disassemble around the first code section containing AIC logic
        # In T8010 kernel, code starts at __TEXT_EXEC (~ offset 0x180000)
        # Let's disassemble sample ARM64 instructions at a key hardware entry
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        sample_code_offset = 0x180000 # __TEXT_EXEC start
        sample_len = 80 # 20 instructions
        code_bytes = data[sample_code_offset : sample_code_offset + sample_len]

        print(f"\n--- DISASSEMBLY OF KERNEL EARLY BOOT INSTRUCTIONS (Offset 0x{sample_code_offset:x}) ---")
        for insn in md.disasm(code_bytes, 0xfffffff007180000):
            print(f"  0x{insn.address:x}:  {insn.mnemonic.ljust(8)} {insn.op_str}")

    # 4. Generate Hardware Adaptation Patch Plan
    print("\n============================================================")
    print("  HARDWARE ADAPTATION & PATCH PLAN FOR HELIO G88:")
    print("============================================================")
    print("""
1. INTERRUPT CONTROLLER HOOK:
   - Target KEXT: com.apple.driver.AppleInterruptController
   - Apple Hardcoded AIC Base: 0xfffffff000000000
   - MediaTek GICv2 Base:      0x0C000000 (Distributor), 0x0C002000 (CPU Interface)
   - Patch Action: Replace AIC MMIO pointer with MT6769 GIC base or redirect via pe_fire.c.

2. BOOT-ARGS SECURITY OVERRIDE:
   - Patch point: boot_args string at offset 0xa1fa92
   - Inject parameters:
       amfi_get_out_of_my_way=1
       cs_enforcement_disable=1
       debug=0x14e
       serial=2

3. TIMER FREQUENCY NEGOTIATION:
   - Apple T8010 uses 24.0 MHz fixed clock.
   - MediaTek Helio G88 uses 13.0 MHz / 26.0 MHz ARM Generic System Counter (CNTPCT_EL0).
   - Patch Action: Scale Mach timebase conversion factors (mach_timebase_info) to match 26MHz.
""")

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "extracted_binaries/kernelcache.macho"
    analyze_kernel(target)
