#!/usr/bin/env python3
"""
DarwinFire: Patch com.apple.datamigrator in ios16_root.img so it always reports
isMigrationNeeded == NO and never launches PreBoard ("Press home to upgrade"),
allowing SpringBoard to boot straight to the iOS 16 Lockscreen / Homescreen!

Patches inside /System/Library/PrivateFrameworks/DataMigration.framework/
XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator (extent blk 1283542):
  1. 0x59a8 (isMigrationNeededWithBuildVersion:lastBuildVersion:clientPID:): mov w0, #0; ret
  2. 0xa098 (waitUntilPreboardDidFinishUnlock): ret
  3. 0xbb00 (isMigrationNeeded:): mov w0, #0; ret

Generates a 12 KB Android sparse delta image `out/skip_preboard.simg`.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS
from hfs_fill_empty import catalog

IMG_PATH = 'ios16_root.img'
OUT_SIMG = 'out/skip_preboard.simg'
DM_PATH = '/System/Library/PrivateFrameworks/DataMigration.framework/XPCServices/com.apple.datamigrator.xpc/com.apple.datamigrator'


def main():
    im = Img(IMG_PATH, False)
    files, path = catalog(im)
    by_path = {path(parent, name): (rec_disk, rec) for rec_disk, parent, name, rec in files}
    _, rec = by_path[DM_PATH]
    s0, c0 = struct.unpack('>II', rec[104:112])
    im.f.close()

    base_off = s0 * BS
    patches = [
        (0x59a8, struct.pack('<II', 0x52800000, 0xd65f03c0), 'isMigrationNeededWithBuildVersion: -> mov w0, #0; ret'),
        (0xa098, struct.pack('<I', 0xd65f03c0), 'waitUntilPreboardDidFinishUnlock -> ret'),
        (0xbb00, struct.pack('<II', 0x52800000, 0xd65f03c0), 'isMigrationNeeded: -> mov w0, #0; ret'),
    ]

    total_blocks = os.path.getsize(IMG_PATH) // BS
    touched = set()
    with open(IMG_PATH, 'r+b') as f:
        for rel, data, desc in patches:
            off = base_off + rel
            f.seek(off)
            f.write(data)
            print(f'[+] Patched {DM_PATH} +{hex(rel)} (img {hex(off)}): {desc}')
            for b in range(off // BS, (off + len(data) - 1) // BS + 1):
                touched.add(b)

        blocks = sorted(touched)
        print(f'[*] Touched blocks: {blocks}')
        chunks, pos = [], 0
        for b in blocks:
            if b > pos:
                chunks.append(('skip', pos, b))
            chunks.append(('raw', b, b + 1))
            pos = b + 1
        if pos < total_blocks:
            chunks.append(('skip', pos, total_blocks))

        os.makedirs(os.path.dirname(OUT_SIMG), exist_ok=True)
        with open(OUT_SIMG, 'wb') as out:
            out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BS, total_blocks, len(chunks), 0))
            for kind, s, e in chunks:
                n = e - s
                if kind == 'skip':
                    out.write(struct.pack('<HHII', 0xCAC3, 0, n, 12))
                else:
                    out.write(struct.pack('<HHII', 0xCAC1, 0, n, 12 + n * BS))
                    f.seek(s * BS)
                    out.write(f.read(n * BS))

    print(f'[+] Wrote delta sparse image: {OUT_SIMG} ({os.path.getsize(OUT_SIMG)} bytes, {len(blocks)} blocks)')


if __name__ == '__main__':
    main()
