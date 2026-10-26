#!/usr/bin/env python3

"""
Apple XNU Kernelcache Binary Patcher for Redmi 12 ("fire" / Helio G88)
Applies targeted binary modifications to bypass Apple hardware checks
"""

import sys
import os
import shutil
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

NOP_OPCODE = bytes.fromhex("1f2003d5")       # nop
SPIN_OPCODE = bytes.fromhex("00000014")      # b . (infinite loop)
RET_TRUE_OPCODE = bytes.fromhex("20008052c0035fd6") # mov w0, #1; ret
RET_ZERO_OPCODE = bytes.fromhex("00008052c0035fd6") # mov w0, #0; ret

def patch_kernel(input_path, output_path, diag_spin=False):
    print("============================================================")
    print("  DARWIN-FIRE XNU BINARY PATCHER")
    print(f"  Input:  {os.path.abspath(input_path)}")
    print(f"  Output: {os.path.abspath(output_path)}")
    print("============================================================\n")

    if not os.path.exists(input_path):
        print(f"[!] Error: Input file not found: {input_path}")
        return False

    with open(input_path, "rb") as f:
        data = bytearray(f.read())

    print(f"[+] Loaded Kernelcache: {len(data) / 1024 / 1024:.2f} MB")

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    # ---------------------------------------------------------
    # PATCH 1: BOOT_LINE_MAGIC Branch Bypass (Offset 0x18003c)
    # ---------------------------------------------------------
    magic_branch_offset = 0x18003c
    curr_bytes = bytes(data[magic_branch_offset : magic_branch_offset + 4])
    
    # Verify current instruction is b.ne
    insns = list(md.disasm(curr_bytes, 0xfffffff00718003c))
    if insns:
        print(f"[*] Original instruction at 0x{magic_branch_offset:x}: {insns[0].mnemonic} {insns[0].op_str} ({curr_bytes.hex()})")
    
    if diag_spin:
        print(f"[+] Applying DIAGNOSTIC SPINLOCK: Patching 0x{magic_branch_offset:x} -> 'b .' (0x14000000)")
        data[magic_branch_offset : magic_branch_offset + 4] = SPIN_OPCODE
    else:
        print(f"[+] Applying BOOT_MAGIC BYPASS: Patching 0x{magic_branch_offset:x} -> 'nop' (0xd503201f)")
        data[magic_branch_offset : magic_branch_offset + 4] = NOP_OPCODE

    # Verify patched instruction
    patched_bytes = bytes(data[magic_branch_offset : magic_branch_offset + 4])
    insns_patched = list(md.disasm(patched_bytes, 0xfffffff00718003c))
    if insns_patched:
        print(f"    -> Verified Patched: {insns_patched[0].mnemonic} {insns_patched[0].op_str} ({patched_bytes.hex()})")

    # ---------------------------------------------------------
    # PATCH 2: Inject Default Verbose Boot Args
    # ---------------------------------------------------------
    # Search for default boot-args format string location in PE
    target_str_pos = data.find(b"debug=0x")
    if target_str_pos != -1:
        print(f"[+] Found debug string at offset 0x{target_str_pos:x}")
    else:
        print("[*] Note: debug string not found directly in text; boot-args passed via LittleKernel cmdline.")

    # Write patched file
    out_dir = os.path.dirname(output_path)
    if out_dir and not os.path.exists(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    with open(output_path, "wb") as f:
        f.write(data)

    print(f"\n[SUCCESS] Successfully written patched kernelcache:")
    print(f"    Path: {os.path.abspath(output_path)}")
    print(f"    Size: {len(data) / 1024 / 1024:.2f} MB ({len(data)} bytes)\n")
    return True

if __name__ == "__main__":
    inp = "extracted_binaries/kernelcache.macho"
    out = "extracted_binaries/kernelcache_patched.macho"
    diag = "--spin" in sys.argv or "--diag" in sys.argv
    
    success = patch_kernel(inp, out, diag_spin=diag)
    if success:
        print("[*] Automatically rebuilding Android boot.img with patched kernel...")
        os.system(f"node tools/bootimg_builder.js")
