#!/usr/bin/env python3
"""Set owner/group 99:99 (macOS "unknown", left by a copy onto an ignore-ownership volume) to 0:0 on every
HFS+ catalog file and folder record, in place. XNU refuses a dyld shared cache not owned by root
(shared_region_map_and_slide_2_np -> EPERM), and launchd/daemons expect root:wheel system files.
Usage: python -I tools/hfs_chown_root.py IMG [--dry-run]"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img

img, dry = sys.argv[1], '--dry-run' in sys.argv
im = Img(img, not dry)
fixed = other = 0
for disk, key, doff, rec in im.leaf_records(im.cat):
    t = struct.unpack('>h', rec[doff:doff + 2])[0]
    if t not in (1, 2):                      # folder / file records; both keep bsdInfo at +32
        continue
    uid, gid = struct.unpack('>II', rec[doff + 32:doff + 40])
    if (uid, gid) == (99, 99):
        if not dry:
            im.f.seek(disk + doff + 32)
            im.f.write(struct.pack('>II', 0, 0))
        fixed += 1
    else:
        other += 1
print('%s %d records 99:99 -> 0:0, %d left as they were' % ('would set' if dry else 'set', fixed, other))
