#!/usr/bin/env python3
"""
Apple UDIF DMG (LZFSE / Raw) to Raw Partition Decompressor
Extracts uncompressed APFS/HFS+ container from Apple DMG images.
Part of Project DarwinFire for Redmi 12.
"""

import sys
import os
import time
import struct
import plistlib
import lzfse

def decompress_dmg(dmg_path, output_raw_path, partition_name="Apple_APFS"):
    print("=" * 60)
    print("  DARWIN-FIRE APPLE DMG LZFSE DECOMPRESSOR")
    print(f"  Source: {dmg_path}")
    print(f"  Target: {output_raw_path}")
    print("=" * 60)

    t0 = time.time()
    file_size = os.path.getsize(dmg_path)
    print(f"[*] Reading DMG koly trailer (file size: {file_size / (1024*1024):.2f} MB)...")

    with open(dmg_path, "rb") as f:
        f.seek(-512, 2)
        koly = f.read(512)
        if koly[:4] != b'koly':
            raise ValueError(f"Invalid DMG: expected 'koly' trailer, got {koly[:4]}")

        xml_offset, xml_length = struct.unpack(">QQ", koly[216:232])
        print(f"[+] XML Plist found at offset {xml_offset} (length: {xml_length} bytes)")

        f.seek(xml_offset)
        plist = plistlib.loads(f.read(xml_length))

        blkx_list = plist.get("resource-fork", {}).get("blkx", [])
        target_blkx = None
        for b in blkx_list:
            if b.get("Name") == partition_name:
                target_blkx = b
                break

        if not target_blkx:
            available = [b.get("Name") for b in blkx_list]
            raise ValueError(f"Partition '{partition_name}' not found. Available: {available}")

        data = target_blkx["Data"]
        magic, version, first_sector, sector_count, data_offset, buffers_needed, block_descriptors = struct.unpack(">4sIIQQII", data[:36])
        if magic != b'mish':
            raise ValueError(f"Expected 'mish' block chunk table, got {magic}")

        print(f"[+] Partition: {partition_name} ({sector_count} sectors = {sector_count * 512 / (1024*1024):.2f} MB)")
        print(f"[+] Total Chunks: {block_descriptors}")

        print(f"[*] Starting streaming decompression to {output_raw_path}...")
        os.makedirs(os.path.dirname(os.path.abspath(output_raw_path)), exist_ok=True)

        zero_block = b'\x00' * (64 * 1024)

        with open(output_raw_path, "wb") as out_f:
            total_bytes_written = 0
            last_report_time = time.time()

            for i in range(block_descriptors):
                entry = data[204 + i*40 : 204 + (i+1)*40]
                if len(entry) < 40:
                    break

                ctype, comment, out_sec, sec_cnt, in_off, in_len = struct.unpack(">IIQQQQ", entry)

                if ctype == 0xffffffff:  # Terminator
                    break

                target_out_bytes = sec_cnt * 512

                if ctype == 0x80000007:  # LZFSE
                    f.seek(in_off)
                    comp_data = f.read(in_len)
                    decomp_data = lzfse.decompress(comp_data)
                    if len(decomp_data) != target_out_bytes:
                        raise ValueError(f"Chunk {i}: decompressed length mismatch: got {len(decomp_data)}, expected {target_out_bytes}")
                    out_f.write(decomp_data)
                    total_bytes_written += len(decomp_data)

                elif ctype == 0x1:  # RAW
                    f.seek(in_off)
                    raw_data = f.read(in_len)
                    out_f.write(raw_data)
                    total_bytes_written += len(raw_data)

                elif ctype == 0x2:  # ZERO / IGNORE
                    rem = target_out_bytes
                    while rem > 0:
                        chunk_zero = min(rem, len(zero_block))
                        out_f.write(zero_block[:chunk_zero])
                        rem -= chunk_zero
                    total_bytes_written += target_out_bytes

                else:
                    print(f"\n[!] Warning: Unknown chunk type {hex(ctype)} at chunk {i}, writing zeros")
                    rem = target_out_bytes
                    while rem > 0:
                        chunk_zero = min(rem, len(zero_block))
                        out_f.write(zero_block[:chunk_zero])
                        rem -= chunk_zero
                    total_bytes_written += target_out_bytes

                now = time.time()
                if now - last_report_time >= 0.5 or i == block_descriptors - 1:
                    pct = (i + 1) / block_descriptors * 100
                    mb_written = total_bytes_written / (1024 * 1024)
                    speed = mb_written / max(0.1, now - t0)
                    print(f"[*] Chunk {i+1}/{block_descriptors} ({pct:.1f}%) | Written: {mb_written:.1f} MB | Speed: {speed:.1f} MB/s", end="\r")
                    last_report_time = now

    dt = time.time() - t0
    final_mb = total_bytes_written / (1024 * 1024)
    print(f"\n[+] DECOMPRESSION COMPLETE in {dt:.1f}s ({final_mb / dt:.1f} MB/s)!")
    print(f"[+] Output file: {output_raw_path} ({final_mb:.2f} MB / {final_mb / 1024:.2f} GB)")

if __name__ == "__main__":
    src = "d:/Code/IOS/extracted_binaries/ios16_x/rootfs.dmg"
    dst = "d:/Code/IOS/extracted_binaries/ios16_x/rootfs_apfs.raw"
    if len(sys.argv) > 1:
        src = sys.argv[1]
    if len(sys.argv) > 2:
        dst = sys.argv[2]
    decompress_dmg(src, dst)

