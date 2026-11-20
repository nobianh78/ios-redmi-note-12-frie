#!/usr/bin/env python3
"""
DarwinFire: Enable com.apple.SpringBoard in /System/Library/xpc/launchd.plist
in-place inside ios16_root.img (keeping size <= existing 121-block extent so no
HFS allocation/bitmap changes are required), AND bundle the clean QuartzCore
SWContext patch into a single tiny delta sparse image `out/quartzcore_sw_sb.simg`.
"""
import os
import plistlib
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS
from hfs_fill_empty import catalog
import patch_dsc_sw_full as qc

IMG_PATH = 'ios16_root.img'
OUT_SIMG = 'out/quartzcore_sw_sb.simg'


def main():
    # 1. First apply clean QuartzCore SWContext patch (no magenta debug bar)
    qc.main()

    # 2. Read /System/Library/xpc/launchd.plist and com.apple.SpringBoard.plist
    im = Img(IMG_PATH, False)
    files, path = catalog(im)
    by_path = {path(parent, name): (rec_disk, rec) for rec_disk, parent, name, rec in files}

    def read_hfs_file(target):
        rec_disk, rec = by_path[target]
        sz = struct.unpack('>Q', rec[88:96])[0]
        exts = [struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i]) for i in range(8)]
        exts = [(s, c) for s, c in exts if c]
        data = bytearray()
        for s, c in exts:
            im.f.seek(s * BS)
            data += im.f.read(c * BS)
        return bytes(data[:sz]), exts, rec_disk

    raw_lp, exts_lp, rec_lp = read_hfs_file('/System/Library/xpc/launchd.plist')
    raw_sb, _, _ = read_hfs_file('/System/Library/LaunchDaemons/com.apple.SpringBoard.plist')
    im.f.close()

    assert len(exts_lp) == 1, f'Expected 1 extent, got {exts_lp}'
    lp_start_blk, lp_blk_cnt = exts_lp[0]
    max_bytes = lp_blk_cnt * BS

    lp = plistlib.loads(raw_lp)
    sb = plistlib.loads(raw_sb)
    sb.pop('LimitLoadFromHardware', None)
    sb.pop('_LimitLoadFromClarityMode', None)
    sb['EmbeddedHomeScreen'] = True
    sb['RunAtLoad'] = False
    sb['KeepAlive'] = True
    lp['LaunchDaemons']['/System/Library/LaunchDaemons/com.apple.SpringBoard.plist'] = sb

    # Trim AppRemovalServices if needed so new launchd.plist fits inside existing 121-block extent
    new_raw = plistlib.dumps(lp, fmt=plistlib.FMT_BINARY)
    if len(new_raw) > max_bytes:
        lp['AppRemovalServices'] = {}
        new_raw = plistlib.dumps(lp, fmt=plistlib.FMT_BINARY)
    if len(new_raw) > max_bytes:
        # Still slightly over? Trim a few unused AppExtensions
        exts_dict = lp.get('AppExtensions', {})
        for k in list(exts_dict.keys()):
            if len(new_raw) <= max_bytes:
                break
            if 'SpringBoard' not in k and 'BackBoard' not in k:
                del exts_dict[k]
                new_raw = plistlib.dumps(lp, fmt=plistlib.FMT_BINARY)

    assert len(new_raw) <= max_bytes, f'Still too big: {len(new_raw)} > {max_bytes}'
    print(f'[+] launchd.plist: {len(raw_lp)} -> {len(new_raw)} bytes (max {max_bytes}, fits in-place @ blk {lp_start_blk}..{lp_start_blk + lp_blk_cnt - 1})')
    print(f'[+] LaunchDaemons ({len(lp["LaunchDaemons"])}): {sorted(lp["LaunchDaemons"].keys())}')

    # 3. Write new launchd.plist in-place and update logicalSize in catalog record
    total_blocks = os.path.getsize(IMG_PATH) // BS
    touched_blocks = set([642803, 642804, 643086, 643150])  # QuartzCore blocks

    with open(IMG_PATH, 'r+b') as f:
        padded = new_raw + b'\x00' * (max_bytes - len(new_raw))
        f.seek(lp_start_blk * BS)
        f.write(padded)
        for b in range(lp_start_blk, lp_start_blk + lp_blk_cnt):
            touched_blocks.add(b)

        # Update HFSPlusForkData.logicalSize (8 bytes big-endian at rec_lp + 88)
        f.seek(rec_lp + 88)
        f.write(struct.pack('>Q', len(new_raw)))
        cat_blk = (rec_lp + 88) // BS
        touched_blocks.add(cat_blk)
        print(f'[+] Updated catalog logicalSize @ {hex(rec_lp + 88)} (blk {cat_blk})')

        blocks = sorted(touched_blocks)
        chunks = []
        pos = 0
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
