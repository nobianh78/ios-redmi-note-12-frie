#!/usr/bin/env python3
"""
DarwinFire: tiny Android sparse image that rewrites only the HFS+ metadata of ios16_root.img (volume
headers, allocation bitmap, catalog, extents B-tree) plus the data of the given files; every other block
is DONT_CARE, so super keeps what is already flashed. For when fastboot cannot push the whole 8 GB.
Only valid if the data blocks of all OTHER files are unchanged since the last full flash.
Usage: python -I tools/make_delta_simg.py IMG OUT.simg /path/in/hfs ...
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hfs_inject_cache import Img, BS
from hfs_fill_empty import catalog


def main():
    img, out, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    im = Img(img, False)
    want = set()
    for fork in (im.alloc, im.cat, im.ext):
        for s, c in fork[1]:
            want.update(range(s, s + c))
    want.update((0, im.total - 1))   # primary (byte 1024) and alternate volume header
    files, path = catalog(im)
    by_path = {path(p, n): rec for _, p, n, rec in files}
    for p in paths:
        if p.startswith('block:'):
            part = p[6:]
            if '-' in part:
                bs, be = map(int, part.split('-'))
                want.update(range(bs, be + 1))
            else:
                want.add(int(part))
            continue
        rec = by_path[p]
        for i in range(8):
            s, c = struct.unpack('>II', rec[104 + 8 * i:112 + 8 * i])
            want.update(range(s, s + c))
    blocks = sorted(want)
    runs, start = [], blocks[0]
    for a, b in zip(blocks, blocks[1:] + [None]):
        if b != a + 1:
            runs.append((start, a + 1))
            start = b
    chunks, pos = [], 0
    for s, e in runs:
        if s > pos:
            chunks.append(('skip', pos, s))
        chunks.append(('raw', s, e))
        pos = e
    if pos < im.total:
        chunks.append(('skip', pos, im.total))
    with open(out, 'wb') as f:
        f.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BS, im.total, len(chunks), 0))
        for kind, s, e in chunks:
            n = e - s
            if kind == 'skip':
                f.write(struct.pack('<HHII', 0xCAC3, 0, n, 12))
            else:
                f.write(struct.pack('<HHII', 0xCAC1, 0, n, 12 + n * BS))
                im.f.seek(s * BS)
                f.write(im.f.read(n * BS))
    print('blocks %d (%.1f MB) in %d runs, %d chunks -> %s' % (len(blocks), len(blocks) * BS / 2**20, len(runs), len(chunks), out))


if __name__ == '__main__':
    main()
