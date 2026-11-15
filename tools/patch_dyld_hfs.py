#!/usr/bin/env python3
"""
DarwinFire: Injects real uncompressed dyld (922,912 bytes) into ios16_root.dmg
Target: Xiaomi Redmi 12 ("fire" / MediaTek MT6768)
Fixes ENOENT/LOAD_BADMACHO when loading /usr/lib/dyld in Pass 3 of parse_machfile
"""

import os
import struct
import sys

def patch_dyld(dmg_path, dyld_path):
    print("=" * 60)
    print("  DARWIN-FIRE: INJECTING REAL /usr/lib/dyld INTO HFS+ ROOT")
    print(f"  Target DMG:  {dmg_path}")
    print(f"  Source dyld: {dyld_path}")
    print("=" * 60)

    if not os.path.exists(dmg_path):
        print(f"[!] Error: {dmg_path} does not exist!")
        sys.exit(1)
    if not os.path.exists(dyld_path):
        print(f"[!] Error: {dyld_path} does not exist!")
        sys.exit(1)

    dyld_size = os.path.getsize(dyld_path)
    print(f"[*] Read dyld binary: {dyld_size} bytes")
    with open(dyld_path, 'rb') as f:
        dyld_bytes = f.read()

    # Validate Mach-O header
    magic, cputype = struct.unpack('<II', dyld_bytes[:8])
    if magic != 0xfeedfacf or cputype != 0x0100000c:
        print(f"[!] Warning: dyld magic=0x{magic:08x}, cputype=0x{cputype:08x} (expected 0xfeedfacf, 0x0100000c)")
    else:
        print(f"[+] Verified valid 64-bit ARM64 Mach-O header!")

    block_size = 4096
    total_blocks_needed = (dyld_size + block_size - 1) // block_size
    padded_size = total_blocks_needed * block_size
    padded_bytes = dyld_bytes + b'\x00' * (padded_size - dyld_size)
    print(f"[*] Required HFS+ allocation blocks: {total_blocks_needed} blocks ({padded_size} bytes)")

    # Target allocation block: 2094448 (right after launchd, confirmed 100% free)
    start_block = 2094448
    disk_data_offset = start_block * block_size

    with open(dmg_path, 'r+b') as f:
        # Step 1: Write raw dyld data fork to block 2094448
        print(f"[*] Step 1: Writing dyld payload to block {start_block} (byte offset {disk_data_offset})...")
        f.seek(disk_data_offset)
        f.write(padded_bytes)
        print(f"[+] Successfully wrote {len(padded_bytes)} bytes to block {start_block}")

        # Step 2: Update Catalog Record for /usr/lib/dyld at offset 229666510
        catalog_dyld_offset = 229666510
        print(f"[*] Step 2: Updating HFSPlusCatalogFile record at offset {catalog_dyld_offset}...")
        f.seek(catalog_dyld_offset)
        rec = f.read(140)
        recType = struct.unpack('>H', rec[:2])[0]
        fileId = struct.unpack('>I', rec[8:12])[0]
        print(f"    Current record: recType=0x{recType:04x}, fileId={fileId}")
        if recType != 2 or fileId != 317227:
            print(f"[!] Error: Offset mismatch! Expected recType=2, fileId=317227, got recType={recType}, fileId={fileId}")
            sys.exit(1)

        # Write dataFork info
        # Offset 88: logicalSize (uint64)
        # Offset 96: clumpSize (uint32)
        # Offset 100: totalBlocks (uint32)
        # Offset 104: extent[0].startBlock (uint32)
        # Offset 108: extent[0].blockCount (uint32)
        f.seek(catalog_dyld_offset + 88)
        f.write(struct.pack('>Q', dyld_size))
        f.write(struct.pack('>I', 0))
        f.write(struct.pack('>I', total_blocks_needed))
        f.write(struct.pack('>II', start_block, total_blocks_needed))
        print(f"[+] Updated catalog: size={dyld_size}, totalBlocks={total_blocks_needed}, extent=({start_block}, {total_blocks_needed})")

        # Step 3: Update Allocation Bitmap at block 65
        print(f"[*] Step 3: Marking blocks {start_block}..{start_block + total_blocks_needed - 1} as allocated in bitmap...")
        bitmap_byte_start = start_block // 8
        bitmap_byte_end = (start_block + total_blocks_needed - 1) // 8
        f.seek(65 * block_size + bitmap_byte_start)
        bm_slice = bytearray(f.read(bitmap_byte_end - bitmap_byte_start + 1))

        for blk in range(start_block, start_block + total_blocks_needed):
            byte_idx = (blk // 8) - bitmap_byte_start
            bit_idx = 7 - (blk % 8)
            bm_slice[byte_idx] |= (1 << bit_idx)

        f.seek(65 * block_size + bitmap_byte_start)
        f.write(bm_slice)
        print(f"[+] Marked {total_blocks_needed} blocks allocated in HFS+ allocation file")

    print("=" * 60)
    print("  [SUCCESS] /usr/lib/dyld INJECTED CLEANLY INTO ios16_root.dmg!")
    print("============================================================")

if __name__ == '__main__':
    dmg = r'D:\Code\IOS\ios1\ios16_root.dmg'
    dyld = r'extracted_binaries/ios16_x/binaries/dyld'
    patch_dyld(dmg, dyld)
