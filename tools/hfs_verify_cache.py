#!/usr/bin/env python3
"""Check every dyld_shared_cache file in the HFS+ image (read through the catalog) against the files
tools/extract_dsc.py decompressed (sha256).
Usage: python -I tools/hfs_verify_cache.py IMG DSC_DIR"""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import BS, CACHE_DIR, Img
im = Img(sys.argv[1], False)
folders = {}
recs = []
for disk, key, doff, rec in im.leaf_records(im.cat):
    parent = struct.unpack('>I', key[2:6])[0]
    name = key[8:8 + 2 * struct.unpack('>H', key[6:8])[0]].decode('utf-16-be', 'replace')
    t = struct.unpack('>h', rec[doff:doff + 2])[0]
    if t == 1:
        folders[(parent, name)] = struct.unpack('>I', rec[doff + 8:doff + 12])[0]
    elif t == 2:
        recs.append((parent, name, rec[doff:doff + 248]))
cnid = 2
for c in CACHE_DIR.split('/'):
    cnid = folders[(cnid, c)]
bad = 0
for parent, name, rec in sorted(recs, key=lambda r: r[1]):
    if parent != cnid or not name.startswith('dyld_shared_cache_arm64') or name.endswith('.symbols'):
        continue
    size = struct.unpack('>Q', rec[88:96])[0]
    ext = [e for e in (struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i]) for i in range(8)) if e[1]]
    h1, left = hashlib.sha256(), size
    for s, c in ext:
        im.f.seek(s * BS)
        n = min(left, c * BS)
        while n:
            chunk = im.f.read(min(n, 8 << 20))
            h1.update(chunk)
            n -= len(chunk)
            left -= len(chunk)
    src = open(os.path.join(sys.argv[2], name), 'rb')
    h2 = hashlib.sha256()
    while True:
        chunk = src.read(8 << 20)
        if not chunk:
            break
        h2.update(chunk)
    ok = h1.digest() == h2.digest() and left == 0 and size > 0
    bad += not ok
    print('%-42s %10d %s' % (name, size, 'OK' if ok else 'MISMATCH'))
f = struct.unpack('>I', im.vh[48:52])[0]
print('freeBlocks %d, totalBlocks %d, %s' % (f, im.total, 'ALL OK' if not bad else '%d BAD' % bad))
sys.exit(1 if bad else 0)
