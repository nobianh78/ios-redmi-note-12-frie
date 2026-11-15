#!/usr/bin/env python3
"""
High-performance pure-Python raw to Android Sparse Image Converter (img2simg)
Converts 6GB raw HFS+ image to Android Sparse Image format (magic 0xED26FF3A).
Bypasses Windows fastboot std::bad_alloc memory limits.
"""

import sys
import os
import struct
import time

SPARSE_HEADER_MAGIC = 0xED26FF3A
MAJOR_VERSION = 1
MINOR_VERSION = 0
FILE_HDR_SIZE = 28
CHUNK_HDR_SIZE = 12
BLOCK_SIZE = 4096

CHUNK_TYPE_RAW = 0xCAC1
CHUNK_TYPE_FILL = 0xCAC2
CHUNK_TYPE_DONT_CARE = 0xCAC3
CHUNK_TYPE_CRC32 = 0xCAC4

def convert_img2simg(raw_path, simg_path, block_size=4096):
    print("=" * 60)
    print("  DARWIN-FIRE ANDROID SPARSE IMAGE CONVERTER (img2simg)")
    print(f"  Source: {raw_path}")
    print(f"  Target: {simg_path}")
    print("=" * 60)

    t0 = time.time()
    raw_size = os.path.getsize(raw_path)
    total_blocks = raw_size // block_size
    if raw_size % block_size != 0:
        total_blocks += 1

    print(f"[*] Input size: {raw_size / (1024*1024):.2f} MB ({total_blocks} blocks of {block_size} bytes)")

    # Pass 1 & 2 combined with in-place streaming
    # We will write chunks directly to output file
    # We leave header for the end or seek back to offset 0
    zero_block = b'\x00' * block_size
    read_blocks = 256  # Read 1 MB at a time
    read_buf_size = read_blocks * block_size

    total_chunks = 0
    total_raw_written = 0
    total_dont_care_blocks = 0

    with open(raw_path, 'rb') as in_f, open(simg_path, 'wb') as out_f:
        # Reserve 28 bytes for Sparse Header
        out_f.write(b'\x00' * FILE_HDR_SIZE)

        current_type = None
        current_data = bytearray()
        current_block_count = 0

        def flush_chunk():
            nonlocal total_chunks, current_type, current_data, current_block_count, total_raw_written, total_dont_care_blocks
            if current_type is None or current_block_count == 0:
                return

            if current_type == CHUNK_TYPE_RAW:
                total_sz = CHUNK_HDR_SIZE + len(current_data)
                hdr = struct.pack('<HHII', CHUNK_TYPE_RAW, 0, current_block_count, total_sz)
                out_f.write(hdr)
                out_f.write(current_data)
                total_raw_written += len(current_data)
            elif current_type == CHUNK_TYPE_DONT_CARE:
                # Zero blocks must be WRITTEN as zeros: DONT_CARE leaves old partition data (Android code on
                # "super") under every all-zero 4K block, which broke dyld's zero-padded pages.
                total_sz = CHUNK_HDR_SIZE + 4
                hdr = struct.pack('<HHII', CHUNK_TYPE_FILL, 0, current_block_count, total_sz)
                out_f.write(hdr)
                out_f.write(b'\x00\x00\x00\x00')
                total_dont_care_blocks += current_block_count

            total_chunks += 1
            current_type = None
            current_data = bytearray()
            current_block_count = 0

        blocks_processed = 0
        last_report = time.time()

        while True:
            buf = in_f.read(read_buf_size)
            if not buf:
                break

            # Process 4096-byte blocks
            for i in range(0, len(buf), block_size):
                blk = buf[i : i + block_size]
                if len(blk) < block_size:
                    blk = blk + b'\x00' * (block_size - len(blk))

                is_zero = (blk == zero_block)
                btype = CHUNK_TYPE_DONT_CARE if is_zero else CHUNK_TYPE_RAW

                # Max chunk size is 65535 blocks (about 256 MB) to keep headers standard
                if current_type == btype and current_block_count < 65535:
                    current_block_count += 1
                    if btype == CHUNK_TYPE_RAW:
                        current_data.extend(blk)
                else:
                    flush_chunk()
                    current_type = btype
                    current_block_count = 1
                    if btype == CHUNK_TYPE_RAW:
                        current_data.extend(blk)

                blocks_processed += 1

            now = time.time()
            if now - last_report >= 0.5:
                pct = blocks_processed / total_blocks * 100
                mb_read = (blocks_processed * block_size) / (1024 * 1024)
                speed = mb_read / max(0.1, now - t0)
                print(f"[*] Processed: {mb_read:.1f}/{raw_size/(1024*1024):.1f} MB ({pct:.1f}%) | Speed: {speed:.1f} MB/s", end="\r")
                last_report = now

        flush_chunk()

        # Seek back and write Sparse Header
        out_f.seek(0)
        file_hdr = struct.pack(
            '<IHHHHIIII',
            SPARSE_HEADER_MAGIC,
            MAJOR_VERSION,
            MINOR_VERSION,
            FILE_HDR_SIZE,
            CHUNK_HDR_SIZE,
            block_size,
            total_blocks,
            total_chunks,
            0  # checksum
        )
        out_f.write(file_hdr)

    dt = time.time() - t0
    final_out_size = os.path.getsize(simg_path)
    print(f"\n[+] SUCCESS: Sparse image created in {dt:.1f}s!")
    print(f"[+] Output: {simg_path}")
    print(f"[+] Sparse Size: {final_out_size / (1024*1024):.2f} MB (Skipped {total_dont_care_blocks * block_size / (1024*1024):.2f} MB empty space)")
    print(f"[+] Total Chunks: {total_chunks}")

if __name__ == "__main__":
    src = "d:/Code/IOS/ios16_root.img"
    dst = "d:/Code/IOS/ios16_root.simg"
    if len(sys.argv) > 1:
        src = sys.argv[1]
    if len(sys.argv) > 2:
        dst = sys.argv[2]
    convert_img2simg(src, dst)
